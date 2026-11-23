// The primary selection over zwp_primary_selection_v1 (see fld/primary.h)
#include "fld/primary.h"

#include <FL/Fl.H>
#include <FL/Fl_Widget.H>
#include <FL/platform.H>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <vector>

#if defined(FLTK_USE_WAYLAND) && defined(FLD_HAVE_PRIMARY_PROTO)
#include <wayland-client.h>
#include "primary-selection-unstable-v1-client-protocol.h"
#define FLD_PRIMARY_WAYLAND 1
#endif

namespace fld {

#ifdef FLD_PRIMARY_WAYLAND
namespace {

const char *const kTextTypes[] = {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"};

struct Offer {
  zwp_primary_selection_offer_v1 *offer = nullptr;
  std::vector<std::string> types;
};

struct State {
  bool tried = false;
  wl_display *dpy = nullptr;
  wl_registry *reg = nullptr;
  wl_seat *seat = nullptr;
  wl_pointer *pointer = nullptr;
  zwp_primary_selection_device_manager_v1 *mgr = nullptr;
  zwp_primary_selection_device_v1 *device = nullptr;
  uint32_t serial = 0;         // the latest input serial, for set_selection
  zwp_primary_selection_source_v1 *source = nullptr; // ours, while we own the selection
  std::string text;            // what our source offers
  Offer *current = nullptr;    // the selection offered to us (maybe our own)
} g;

// a paste waiting for another program to send the text
struct Pending {
  int fd = -1;
  std::string data;
  Fl_Widget_Tracker *target = nullptr;
};
Pending *g_pending = nullptr;

void deliver(Fl_Widget *w, const std::string &text) {
  if (!w || text.empty()) return;
  Fl::e_text = (char *)text.c_str();
  Fl::e_length = (int)text.size();
  Fl::e_clipboard_type = Fl::clipboard_plain_text;
  w->handle(FL_PASTE);
  Fl::e_text = nullptr;
  Fl::e_length = 0;
}

void finish_pending() {
  Pending *p = g_pending;
  g_pending = nullptr;
  Fl::remove_fd(p->fd);
  close(p->fd);
  if (p->target && p->target->exists()) deliver(p->target->widget(), p->data);
  delete p->target;
  delete p;
}

void read_cb(int fd, void *) {
  if (!g_pending || g_pending->fd != fd) {
    Fl::remove_fd(fd);
    return;
  }
  char buf[8192];
  for (;;) {
    ssize_t n = read(fd, buf, sizeof buf);
    if (n > 0) {
      g_pending->data.append(buf, (size_t)n);
      if (g_pending->data.size() > (64u << 20)) break; // a sane limit
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && errno == EAGAIN) return; // more later
    break; // end of data or an error
  }
  finish_pending();
}

// ---- offers
void offer_type(void *data, zwp_primary_selection_offer_v1 *, const char *mime) {
  ((Offer *)data)->types.push_back(mime);
}
const zwp_primary_selection_offer_v1_listener kOffer = {offer_type};

void drop_offer(Offer *o) {
  if (!o) return;
  zwp_primary_selection_offer_v1_destroy(o->offer);
  delete o;
}

// ---- the device
void dev_data_offer(void *, zwp_primary_selection_device_v1 *, zwp_primary_selection_offer_v1 *offer) {
  Offer *o = new Offer;
  o->offer = offer;
  zwp_primary_selection_offer_v1_add_listener(offer, &kOffer, o);
}
void dev_selection(void *, zwp_primary_selection_device_v1 *, zwp_primary_selection_offer_v1 *offer) {
  Offer *o = offer ? (Offer *)zwp_primary_selection_offer_v1_get_user_data(offer) : nullptr;
  if (o != g.current) drop_offer(g.current);
  g.current = o;
}
const zwp_primary_selection_device_v1_listener kDevice = {dev_data_offer, dev_selection};

// ---- our source
void src_send(void *, zwp_primary_selection_source_v1 *, const char *, int32_t fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
  size_t off = 0;
  while (off < g.text.size()) {
    ssize_t n = write(fd, g.text.data() + off, g.text.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    off += (size_t)n;
  }
  close(fd);
}
void src_cancelled(void *, zwp_primary_selection_source_v1 *src) {
  if (src == g.source) {
    g.source = nullptr;
    g.text.clear();
  }
  zwp_primary_selection_source_v1_destroy(src);
}
const zwp_primary_selection_source_v1_listener kSource = {src_send, src_cancelled};

// ---- input serials (a selection needs a recent one)
void p_enter(void *, wl_pointer *, uint32_t serial, wl_surface *, wl_fixed_t, wl_fixed_t) { g.serial = serial; }
void p_leave(void *, wl_pointer *, uint32_t, wl_surface *) {}
void p_motion(void *, wl_pointer *, uint32_t, wl_fixed_t, wl_fixed_t) {}
void p_button(void *, wl_pointer *, uint32_t serial, uint32_t, uint32_t, uint32_t) { g.serial = serial; }
void p_axis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
void p_frame(void *, wl_pointer *) {}
void p_axis_source(void *, wl_pointer *, uint32_t) {}
void p_axis_stop(void *, wl_pointer *, uint32_t, uint32_t) {}
void p_axis_discrete(void *, wl_pointer *, uint32_t, int32_t) {}
const wl_pointer_listener kPointer = {p_enter, p_leave, p_motion, p_button, p_axis,
                                      p_frame, p_axis_source, p_axis_stop, p_axis_discrete};

void seat_caps(void *, wl_seat *seat, uint32_t caps) {
  bool has = caps & WL_SEAT_CAPABILITY_POINTER;
  if (has && !g.pointer) {
    g.pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(g.pointer, &kPointer, nullptr);
  } else if (!has && g.pointer) {
    wl_pointer_destroy(g.pointer);
    g.pointer = nullptr;
  }
}
void seat_name(void *, wl_seat *, const char *) {}
const wl_seat_listener kSeat = {seat_caps, seat_name};

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  if (!strcmp(iface, wl_seat_interface.name) && !g.seat) {
    g.seat = (wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, std::min(version, 5u));
    wl_seat_add_listener(g.seat, &kSeat, nullptr);
  } else if (!strcmp(iface, zwp_primary_selection_device_manager_v1_interface.name)) {
    g.mgr = (zwp_primary_selection_device_manager_v1 *)wl_registry_bind(
        reg, name, &zwp_primary_selection_device_manager_v1_interface, 1);
  }
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kRegistry = {reg_global, reg_remove};

bool setup() {
  if (g.tried) return g.device != nullptr;
  wl_display *dpy = fl_wl_display();
  if (!dpy) return false; // X11, or no window yet
  g.tried = true;
  g.dpy = dpy;
  g.reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(g.reg, &kRegistry, nullptr);
  wl_display_roundtrip(dpy); // the globals
  wl_display_roundtrip(dpy); // the seat's capabilities
  if (g.seat && g.mgr) {
    g.device = zwp_primary_selection_device_manager_v1_get_device(g.mgr, g.seat);
    zwp_primary_selection_device_v1_add_listener(g.device, &kDevice, nullptr);
    wl_display_roundtrip(dpy); // the current selection, and a pointer serial
  }
  return g.device != nullptr;
}

} // namespace
#endif // FLD_PRIMARY_WAYLAND

bool primary_native() {
#ifdef FLD_PRIMARY_WAYLAND
  return setup();
#else
  return false;
#endif
}

void primary_copy(const std::string &text) {
  if (text.empty()) return;
  Fl::copy(text.c_str(), (int)text.size(), 0); // FLTK's own (Fl_Input and friends)
#ifdef FLD_PRIMARY_WAYLAND
  if (!setup()) return;
  zwp_primary_selection_source_v1 *src = zwp_primary_selection_device_manager_v1_create_source(g.mgr);
  zwp_primary_selection_source_v1_add_listener(src, &kSource, nullptr);
  for (const char *t : kTextTypes) zwp_primary_selection_source_v1_offer(src, t);
  // the old source is cancelled by the compositor and destroyed then
  g.source = src;
  g.text = text;
  zwp_primary_selection_device_v1_set_selection(g.device, src, g.serial);
  wl_display_flush(g.dpy);
#endif
}

void primary_paste(Fl_Widget &w) {
#ifdef FLD_PRIMARY_WAYLAND
  if (setup()) {
    // our own selection: no round trip through the compositor
    if (g.source) {
      deliver(&w, g.text);
      return;
    }
    if (!g.current || g_pending) return;
    const char *type = nullptr;
    for (const char *t : kTextTypes) {
      if (std::find(g.current->types.begin(), g.current->types.end(), t) != g.current->types.end()) {
        type = t;
        break;
      }
    }
    if (!type) return;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC | O_NONBLOCK) != 0) return;
    zwp_primary_selection_offer_v1_receive(g.current->offer, type, fds[1]);
    close(fds[1]);
    wl_display_flush(g.dpy);
    g_pending = new Pending;
    g_pending->fd = fds[0];
    g_pending->target = new Fl_Widget_Tracker(&w);
    Fl::add_fd(fds[0], FL_READ, read_cb);
    return;
  }
#endif
  Fl::paste(w, 0);
}

} // namespace fld
