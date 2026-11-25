#include "wl_present.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <wayland-client.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <utility>

#include "linux-dmabuf-v1-client-protocol.h"
#include "viewporter-client-protocol.h"

namespace web {
namespace wl_present {
namespace {

wl_subcompositor *g_subcomp = nullptr;
zwp_linux_dmabuf_v1 *g_dmabuf = nullptr;
wp_viewporter *g_viewporter = nullptr;
std::set<std::pair<uint32_t, uint64_t>> g_formats; // what the compositor takes
std::set<Presenter *> g_presenters;
std::map<Fl_Window *, Fl_Window *> g_overlays;
int g_state = 0; // 0 untried, 1 available, -1 not

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  if (!strcmp(iface, wl_subcompositor_interface.name))
    g_subcomp = (wl_subcompositor *)wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
  else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name) && version >= 3)
    // version 3: the formats and modifiers come as events right after binding
    g_dmabuf = (zwp_linux_dmabuf_v1 *)wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface, 3);
  else if (!strcmp(iface, wp_viewporter_interface.name))
    g_viewporter = (wp_viewporter *)wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kRegistry = {reg_global, reg_remove};

void dmabuf_format(void *, zwp_linux_dmabuf_v1 *, uint32_t) {}
void dmabuf_modifier(void *, zwp_linux_dmabuf_v1 *, uint32_t format, uint32_t hi, uint32_t lo) {
  g_formats.insert({format, ((uint64_t)hi << 32) | lo});
}
const zwp_linux_dmabuf_v1_listener kDmabuf = {dmabuf_format, dmabuf_modifier};

// buffer creation result (on a queue of our own)
struct Made {
  wl_buffer *buffer = nullptr;
  bool done = false;
};
void params_created(void *d, zwp_linux_buffer_params_v1 *, wl_buffer *b) {
  ((Made *)d)->buffer = b;
  ((Made *)d)->done = true;
}
void params_failed(void *d, zwp_linux_buffer_params_v1 *) { ((Made *)d)->done = true; }
const zwp_linux_buffer_params_v1_listener kParams = {params_created, params_failed};

void buffer_release(void *d, wl_buffer *) {
  auto *e = (Presenter::Entry *)d;
  if (e->owner) e->owner->buffer_released(e);
}
const wl_buffer_listener kBuffer = {buffer_release};

void frame_done_cb(void *d, wl_callback *, uint32_t) { ((Presenter *)d)->frame_done(); }
const wl_callback_listener kFrame = {frame_done_cb};

// Makes a wl_buffer for a DMA-BUF. Waits for the compositor's answer (the
// first frames of each buffer only: they are kept), so a buffer it cannot
// import is a "failed" event and not a protocol error that ends fl-web.
wl_buffer *make_buffer(const Dmabuf &b) {
  wl_display *dpy = fl_wl_display();
  wl_event_queue *q = wl_display_create_queue(dpy);
  auto *wrapped = (zwp_linux_dmabuf_v1 *)wl_proxy_create_wrapper(g_dmabuf);
  wl_proxy_set_queue((wl_proxy *)wrapped, q);
  zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(wrapped);
  wl_proxy_wrapper_destroy(wrapped);
  Made made;
  zwp_linux_buffer_params_v1_add_listener(params, &kParams, &made);
  for (int i = 0; i < b.planes; i++)
    zwp_linux_buffer_params_v1_add(params, b.fd[i], (uint32_t)i, b.offset[i], b.stride[i],
                                   (uint32_t)(b.modifier >> 32), (uint32_t)(b.modifier & 0xffffffff));
  zwp_linux_buffer_params_v1_create(params, b.width, b.height, b.fourcc, 0);
  while (!made.done)
    if (wl_display_roundtrip_queue(dpy, q) < 0) break;
  zwp_linux_buffer_params_v1_destroy(params);
  // its release events go to FLTK's queue, which dispatches them
  if (made.buffer) wl_proxy_set_queue((wl_proxy *)made.buffer, nullptr);
  wl_event_queue_destroy(q);
  return made.buffer;
}

} // namespace

bool available() {
  if (g_state) return g_state > 0;
  g_state = -1;
  const char *rb = getenv("FL_WEB_READBACK");
  if (rb && *rb && strcmp(rb, "0")) return false;
  fl_open_display();
  wl_display *dpy = fl_wl_display();
  if (!dpy || !fl_wl_compositor()) return false;
  // a queue of our own: a roundtrip on the default one would dispatch FLTK's
  // events from inside whatever called us
  wl_event_queue *q = wl_display_create_queue(dpy);
  auto *wrapped = (wl_display *)wl_proxy_create_wrapper(dpy);
  wl_proxy_set_queue((wl_proxy *)wrapped, q);
  wl_registry *reg = wl_display_get_registry(wrapped);
  wl_registry_add_listener(reg, &kRegistry, nullptr);
  wl_display_roundtrip_queue(dpy, q);
  if (g_dmabuf) {
    zwp_linux_dmabuf_v1_add_listener(g_dmabuf, &kDmabuf, nullptr);
    wl_display_roundtrip_queue(dpy, q); // the formats
  }
  wl_registry_destroy(reg);
  wl_proxy_wrapper_destroy(wrapped);
  for (wl_proxy *p : {(wl_proxy *)g_subcomp, (wl_proxy *)g_dmabuf, (wl_proxy *)g_viewporter})
    if (p) wl_proxy_set_queue(p, nullptr);
  wl_event_queue_destroy(q);
  if (g_subcomp && g_dmabuf && g_viewporter && !g_formats.empty()) g_state = 1;
  return g_state > 0;
}

bool supports(uint32_t fourcc, uint64_t modifier) { return g_formats.count({fourcc, modifier}) > 0; }

void forget(const void *key) {
  for (Presenter *p : g_presenters) p->drop(key);
}

void set_overlay(Fl_Window *top, Fl_Window *overlay) {
  if (overlay) g_overlays[top] = overlay;
  else g_overlays.erase(top);
}

void overlays_changed(Fl_Window *top) {
  for (Presenter *p : g_presenters) p->restack_in(top);
}

void pass_input(Fl_Window *w) {
  if (!fl_wl_display() || !w->shown() || !fl_wl_compositor()) return;
  wld_window *xid = fl_wl_xid(w);
  wl_surface *s = xid ? fl_wl_surface(xid) : nullptr;
  if (!s) return;
  wl_region *none = wl_compositor_create_region(fl_wl_compositor());
  wl_surface_set_input_region(s, none); // applied with FLTK's next commit of it
  wl_region_destroy(none);
}

// ---- Presenter ------------------------------------------------------------------

Presenter::Presenter() { g_presenters.insert(this); }

Presenter::~Presenter() {
  g_presenters.erase(this);
  Fl::remove_timeout(shown_later, this);
  if (frame_cb_) {
    wl_callback_destroy(frame_cb_);
    frame_cb_ = nullptr;
  }
  destroy_surface();
  // WebKit gets everything back
  if (shown_) {
    auto s = std::move(shown_);
    shown_ = nullptr;
    s();
  }
  for (auto &kv : entries_) {
    Entry *e = kv.second;
    e->owner = nullptr;
    if (e->release) {
      auto r = std::move(e->release);
      e->release = nullptr;
      r();
    }
    if (e->buffer) wl_buffer_destroy(e->buffer);
    delete e;
  }
  entries_.clear();
  if (wl_display *dpy = fl_wl_display()) wl_display_flush(dpy);
}

void Presenter::shown_later(void *d) {
  Presenter *p = (Presenter *)d;
  if (!p->shown_) return;
  auto s = std::move(p->shown_);
  p->shown_ = nullptr;
  s();
}

void Presenter::destroy_surface() {
  if (frame_cb_) {
    // that frame will not be shown: WebKit goes on anyway
    wl_callback_destroy(frame_cb_);
    frame_cb_ = nullptr;
    Fl::add_timeout(0, shown_later, this);
  }
  if (viewport_) wp_viewport_destroy(viewport_);
  if (sub_) wl_subsurface_destroy(sub_);
  if (surface_) wl_surface_destroy(surface_);
  viewport_ = nullptr;
  sub_ = nullptr;
  surface_ = nullptr;
  parent_ = nullptr;
  placed_x_ = placed_y_ = dest_w_ = dest_h_ = -1;
  // nothing is on screen any more: all but the current frame go back
  for (auto &kv : entries_) kv.second->attached = false;
  give_back_others();
}

// every buffer the compositor no longer holds, but the current one, back to WebKit
void Presenter::give_back_others() {
  for (auto &kv : entries_) {
    Entry *e = kv.second;
    if (e == current_ || e->attached || !e->release) continue;
    auto r = std::move(e->release);
    e->release = nullptr;
    r();
  }
}

bool Presenter::ensure_surface() {
  if (!win_ || !win_->shown()) return false;
  wld_window *xid = fl_wl_xid(win_);
  wl_surface *p = xid ? fl_wl_surface(xid) : nullptr;
  if (!p) return false;
  if (p == parent_ && surface_) return true;
  // a new window surface (FLTK makes one each time a window is shown)
  destroy_surface();
  surface_ = wl_compositor_create_surface(fl_wl_compositor());
  // the pointer and keyboard stay with the window: FLTK handles them
  wl_region *none = wl_compositor_create_region(fl_wl_compositor());
  wl_surface_set_input_region(surface_, none);
  wl_region_destroy(none);
  sub_ = wl_subcompositor_get_subsurface(g_subcomp, surface_, p);
  wl_subsurface_set_desync(sub_); // frames go on screen without the window's commits
  viewport_ = wp_viewporter_get_viewport(g_viewporter, surface_);
  parent_ = p;
  restack();
  return true;
}

void Presenter::restack_in(Fl_Window *top) {
  if (!sub_ || !win_ || win_->top_window() != top) return;
  restack();
  wl_surface_commit(parent_); // the order is the parent's state
  wl_display_flush(fl_wl_display());
}

void Presenter::restack() {
  if (!sub_ || !win_) return;
  auto it = g_overlays.find(win_->top_window());
  if (it == g_overlays.end()) return;
  Fl_Window *o = it->second;
  if (!o->shown() || !o->visible()) return;
  wld_window *xid = fl_wl_xid(o);
  if (wl_surface *s = xid ? fl_wl_surface(xid) : nullptr) wl_subsurface_place_below(sub_, s);
}

void Presenter::place(Fl_Window *win, int x, int y, int w, int h) {
  win_ = win;
  x_ = x;
  y_ = y;
  w_ = w;
  h_ = h;
  if (!mapped_ || !current_) return;
  bool fresh = !surface_ || (win_->shown() && fl_wl_xid(win_) && fl_wl_surface(fl_wl_xid(win_)) != parent_);
  if (!ensure_surface()) return;
  if (fresh) {
    attach_current(true, {});
  } else {
    // position and size only; the position takes effect with the window's next commit
    attach_current(false, {});
  }
}

// geometry, and the current buffer when `full` or there is damage
void Presenter::attach_current(bool full, const std::vector<Rect> &damage) {
  if (!surface_) return;
  float s = Fl::screen_scale(win_->screen_num());
  int px = (int)lround(x_ * s), py = (int)lround(y_ * s);
  int dw = std::max(1, (int)lround(w_ * s)), dh = std::max(1, (int)lround(h_ * s));
  bool changed = false;
  if (px != placed_x_ || py != placed_y_) {
    wl_subsurface_set_position(sub_, px, py);
    placed_x_ = px;
    placed_y_ = py;
    wl_surface_commit(parent_); // a subsurface moves with its parent's commit
  }
  if (dw != dest_w_ || dh != dest_h_) {
    wp_viewport_set_destination(viewport_, dw, dh);
    dest_w_ = dw;
    dest_h_ = dh;
    changed = true;
  }
  if (full || !damage.empty()) {
    wl_surface_attach(surface_, current_ ? current_->buffer : nullptr, 0, 0);
    if (current_) current_->attached = true;
    if (full) wl_surface_damage_buffer(surface_, 0, 0, INT32_MAX, INT32_MAX);
    else
      for (const Rect &r : damage) wl_surface_damage_buffer(surface_, r.x, r.y, r.w, r.h);
    changed = true;
  }
  if (changed) wl_surface_commit(surface_);
  wl_display_flush(fl_wl_display());
}

void Presenter::set_mapped(bool on) {
  if (on == mapped_) return;
  mapped_ = on;
  if (!on) {
    if (surface_) {
      if (frame_cb_) {
        wl_callback_destroy(frame_cb_);
        frame_cb_ = nullptr;
        Fl::add_timeout(0, shown_later, this);
      }
      // unmapped; the compositor gives the buffer back, but it stays ours to
      // show again (buffer_released keeps the current one)
      wl_surface_attach(surface_, nullptr, 0, 0);
      wl_surface_commit(surface_);
      wl_display_flush(fl_wl_display());
    }
    return;
  }
  if (current_ && ensure_surface()) {
    restack();
    attach_current(true, {});
  }
}

bool Presenter::show(const void *key, const Dmabuf &buf, const std::vector<Rect> &damage, bool all,
                     std::function<void()> shown, std::function<void()> release) {
  Entry *e;
  auto it = entries_.find(key);
  if (it != entries_.end()) {
    e = it->second;
  } else {
    if (!supports(buf.fourcc, buf.modifier)) return false;
    wl_buffer *b = make_buffer(buf);
    if (!b) return false;
    e = new Entry;
    e->buffer = b;
    e->owner = this;
    e->key = key;
    wl_buffer_add_listener(b, &kBuffer, e);
    entries_[key] = e;
  }
  if (e->release) {
    // handed over again before it was given back: give it back first
    auto r = std::move(e->release);
    e->release = nullptr;
    r();
  }
  e->release = std::move(release);
  Entry *prev = current_;
  current_ = e;
  if (shown_) {
    // the frame before was never confirmed: let it go
    Fl::remove_timeout(shown_later, this);
    auto s = std::move(shown_);
    shown_ = nullptr;
    s();
  }
  shown_ = std::move(shown);
  if (mapped_ && ensure_surface()) {
    if (frame_cb_) wl_callback_destroy(frame_cb_);
    frame_cb_ = wl_surface_frame(surface_);
    wl_callback_add_listener(frame_cb_, &kFrame, this);
    // WebKit's damage is a hint about what changed since its frame before
    attach_current(all || damage.empty() || !prev, damage);
    // the buffer before comes back with its release event, unless the
    // compositor gave it back already
    give_back_others();
  } else {
    // not on screen: the newest frame is kept for when it is; the one before
    // goes back to WebKit, and WebKit goes on at a slow pace
    give_back_others();
    Fl::remove_timeout(shown_later, this);
    Fl::add_timeout(0.5, shown_later, this);
  }
  return true;
}

void Presenter::buffer_released(Entry *e) {
  e->attached = false;
  // the current frame stays ours (shown again when mapped); the others go back
  if (e == current_ || !e->release) return;
  auto r = std::move(e->release);
  e->release = nullptr;
  r();
}

void Presenter::frame_done() {
  if (frame_cb_) {
    wl_callback_destroy(frame_cb_);
    frame_cb_ = nullptr;
  }
  if (!shown_) return;
  auto s = std::move(shown_);
  shown_ = nullptr;
  s();
}

void Presenter::drop(const void *key) {
  auto it = entries_.find(key);
  if (it == entries_.end()) return;
  Entry *e = it->second;
  entries_.erase(it);
  if (e == current_) {
    current_ = nullptr;
    if (surface_) {
      wl_surface_attach(surface_, nullptr, 0, 0);
      wl_surface_commit(surface_);
    }
  }
  if (e->buffer) wl_buffer_destroy(e->buffer);
  delete e;
}

} // namespace wl_present
} // namespace web
