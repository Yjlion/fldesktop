// Drags with a drag image (see fld/drag.h)
#include "fld/drag.h"

#include <FL/Fl.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>

#include <algorithm>
#include <atomic>
#include <cairo.h>
#include <cerrno>
#include <cmath>
#include <memory>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#if defined(FLTK_USE_WAYLAND)
#include <wayland-client.h>
#define FLD_DRAG_WAYLAND 1
#endif

#include "fld/theme.h"

namespace fld {

// ---------------------------------------------------------------- pixels

static void color_rgb(Fl_Color c, double &r, double &g, double &b) {
  uchar R, G, B;
  Fl::get_color(c, R, G, B);
  r = R / 255.0;
  g = G / 255.0;
  b = B / 255.0;
}

// an icon as a premultiplied ARGB cairo surface of px x px (nullptr: no pixels)
static cairo_surface_t *icon_surface(Fl_Image *icon, int px) {
  if (!icon || px <= 0) return nullptr;
  Fl_Image *c = icon->copy(px, px);
  if (!c) return nullptr;
  Fl_Image *img = c;
  if (Fl_Shared_Image *sh = c->as_shared_image()) img = (Fl_Image *)sh->image();
  Fl_RGB_Image *rgb = img ? dynamic_cast<Fl_RGB_Image *>(img) : nullptr;
  cairo_surface_t *s = nullptr;
  if (rgb) {
    rgb->normalize(); // an SVG image draws its pixels now
    int w = rgb->data_w(), h = rgb->data_h(), d = rgb->d();
    if (rgb->count() >= 1 && rgb->data() && rgb->data()[0] && w > 0 && h > 0 && d >= 1 && d <= 4) {
      int ld = rgb->ld() ? rgb->ld() : w * d;
      const uchar *src = (const uchar *)rgb->data()[0];
      s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
      cairo_surface_flush(s);
      unsigned char *dst = cairo_image_surface_get_data(s);
      int stride = cairo_image_surface_get_stride(s);
      for (int y = 0; y < h; y++) {
        const uchar *p = src + y * ld;
        uint32_t *q = (uint32_t *)(dst + y * stride);
        for (int x = 0; x < w; x++, p += d) {
          unsigned r, g, b, a;
          if (d >= 3) { r = p[0]; g = p[1]; b = p[2]; a = d == 4 ? p[3] : 255; }
          else { r = g = b = p[0]; a = d == 2 ? p[1] : 255; }
          q[x] = (a << 24) | ((r * a / 255) << 16) | ((g * a / 255) << 8) | (b * a / 255);
        }
      }
      cairo_surface_mark_dirty(s);
    }
  }
  delete c;
  return s;
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
  cairo_new_sub_path(cr);
  cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
  cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
  cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
  cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
  cairo_close_path(cr);
}

static void badge(cairo_t *cr, double cx, double cy, double rad, Fl_Color bg, const std::string &text) {
  double r, g, b;
  cairo_arc(cr, cx, cy, rad, 0, 2 * M_PI);
  color_rgb(bg, r, g, b);
  cairo_set_source_rgb(cr, r, g, b);
  cairo_fill_preserve(cr);
  cairo_set_source_rgba(cr, 1, 1, 1, 0.9);
  cairo_set_line_width(cr, 1.5);
  cairo_stroke(cr);
  cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, rad * (text.size() > 2 ? 0.9 : 1.15));
  cairo_text_extents_t te;
  cairo_text_extents(cr, text.c_str(), &te);
  cairo_move_to(cr, cx - te.width / 2 - te.x_bearing, cy - te.height / 2 - te.y_bearing);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_show_text(cr, text.c_str());
}

DragPixels render_drag_image(const DragImage &img, int scale, bool copy_badge) {
  DragPixels out;
  scale = std::max(1, scale);
  const Palette &pal = palette();
  std::vector<cairo_surface_t *> icons;
  int S = std::max(16, img.size);
  for (size_t i = 0; i < img.icons.size() && icons.size() < 3; i++)
    if (cairo_surface_t *s = icon_surface(img.icons[i], S * scale)) icons.push_back(s);

  const int R = 11, OFF = 7; // badge radius, offset of each icon behind the front one
  int W, H;
  std::string label = img.label;
  double chip_w = 0, chip_h = 28;
  cairo_surface_t *probe = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
  cairo_t *pc = cairo_create(probe);
  cairo_select_font_face(pc, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(pc, 13);
  if (icons.empty()) {
    if (label.empty()) label = img.count > 1 ? std::to_string(img.count) + " items" : "1 item";
    // shorten long labels (on UTF-8 character boundaries)
    cairo_text_extents_t te;
    cairo_text_extents(pc, label.c_str(), &te);
    bool cut = false;
    while (te.x_advance > 220 && label.size() > 1) {
      size_t n = label.size() - 1;
      while (n > 0 && ((unsigned char)label[n] & 0xC0) == 0x80) n--;
      label.erase(n);
      cut = true;
      cairo_text_extents(pc, (label + "\xE2\x80\xA6").c_str(), &te);
    }
    if (cut) label += "\xE2\x80\xA6";
    chip_w = std::max(60.0, te.x_advance + 28);
    W = (int)chip_w + R + 2;
    H = (int)chip_h + R + 2;
  } else {
    int n = (int)icons.size();
    W = S + (n - 1) * OFF + R + 2;
    H = S + (n - 1) * OFF + R + 2;
  }
  cairo_destroy(pc);
  cairo_surface_destroy(probe);

  cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, W * scale, H * scale);
  cairo_t *cr = cairo_create(surf);
  cairo_scale(cr, scale, scale);
  double r, g, b;
  double fx, fy; // the front icon's (or the chip's) top left corner
  if (icons.empty()) {
    fx = 1;
    fy = R;
    rounded_rect(cr, fx, fy, chip_w, chip_h, 7);
    color_rgb(pal.bg2, r, g, b);
    cairo_set_source_rgba(cr, r, g, b, 0.94);
    cairo_fill_preserve(cr);
    color_rgb(pal.accent, r, g, b);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_set_line_width(cr, 1.5);
    cairo_stroke(cr);
    color_rgb(pal.fg, r, g, b);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 13);
    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    cairo_move_to(cr, fx + 14, fy + chip_h / 2 + (fe.ascent - fe.descent) / 2);
    cairo_show_text(cr, label.c_str());
    out.hot_x = (int)(fx + 14);
    out.hot_y = (int)(fy + chip_h / 2);
  } else {
    int n = (int)icons.size();
    // back to front: icon k sits k*OFF right of and above the front one
    fx = 0;
    fy = R + (n - 1) * OFF;
    for (int k = n - 1; k >= 0; k--) {
      cairo_save(cr);
      cairo_translate(cr, fx + k * OFF, fy - k * OFF);
      cairo_scale(cr, 1.0 / scale, 1.0 / scale);
      cairo_set_source_surface(cr, icons[k], 0, 0);
      cairo_paint_with_alpha(cr, k == 0 ? 0.88 : 0.55);
      cairo_restore(cr);
    }
    out.hot_x = (int)(fx + S / 2);
    out.hot_y = (int)(fy + S / 2);
  }
  if (img.count > 1) {
    double bx = icons.empty() ? fx + chip_w : fx + S + (icons.size() - 1) * OFF;
    badge(cr, std::min<double>(bx, W - R - 1), R + 0.5, R, pal.accent,
          img.count > 999 ? "999+" : std::to_string(img.count));
  }
  if (copy_badge) {
    double bx = icons.empty() ? fx + chip_w : fx + S;
    double by = icons.empty() ? fy + chip_h : fy + S;
    badge(cr, std::min<double>(bx - 4, W - 10), std::min<double>(by - 4, H - 10), 8, fl_rgb_color(46, 160, 67), "+");
  }
  cairo_destroy(cr);
  cairo_surface_flush(surf);
  out.w = W * scale;
  out.h = H * scale;
  out.argb.resize((size_t)out.w * out.h);
  int stride = cairo_image_surface_get_stride(surf);
  const unsigned char *data = cairo_image_surface_get_data(surf);
  for (int y = 0; y < out.h; y++) memcpy(&out.argb[(size_t)y * out.w], data + y * stride, out.w * 4);
  cairo_surface_destroy(surf);
  for (auto *s : icons) cairo_surface_destroy(s);
  return out;
}

// ---------------------------------------------------------------- Wayland

#ifdef FLD_DRAG_WAYLAND
namespace {

struct Seat {
  wl_display *dpy = nullptr;
  wl_registry *reg = nullptr;
  wl_seat *seat = nullptr;
  wl_pointer *pointer = nullptr;
  wl_data_device_manager *mgr = nullptr;
  uint32_t mgr_ver = 0;
  wl_shm *shm = nullptr;
  wl_data_device *device = nullptr;
  uint32_t press_serial = 0;
  int buttons = 0;
  // offers the compositor gives our own data device: kept until the next one
  // (destroying a drag offer before its drag ended would cancel that drag)
  std::vector<wl_data_offer *> drag_offers;
  wl_data_offer *pending = nullptr, *selection = nullptr;
  bool tried = false;
} g;

// pointer: only the serial of the button press a drag starts from
void p_enter(void *, wl_pointer *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t) {}
void p_leave(void *, wl_pointer *, uint32_t, wl_surface *) { g.buttons = 0; }
void p_motion(void *, wl_pointer *, uint32_t, wl_fixed_t, wl_fixed_t) {}
void p_button(void *, wl_pointer *, uint32_t serial, uint32_t, uint32_t, uint32_t state) {
  if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
    g.press_serial = serial;
    g.buttons++;
  } else if (g.buttons > 0) g.buttons--;
}
void p_axis(void *, wl_pointer *, uint32_t, uint32_t, wl_fixed_t) {}
void p_frame(void *, wl_pointer *) {}
void p_axis_source(void *, wl_pointer *, uint32_t) {}
void p_axis_stop(void *, wl_pointer *, uint32_t, uint32_t) {}
void p_axis_discrete(void *, wl_pointer *, uint32_t, int32_t) {}
const wl_pointer_listener kPointer = {p_enter, p_leave, p_motion, p_button, p_axis,
                                      p_frame, p_axis_source, p_axis_stop, p_axis_discrete};

void seat_caps(void *, wl_seat *s, uint32_t caps) {
  if ((caps & WL_SEAT_CAPABILITY_POINTER) && !g.pointer) {
    g.pointer = wl_seat_get_pointer(s);
    wl_pointer_add_listener(g.pointer, &kPointer, nullptr);
  } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && g.pointer) {
    wl_pointer_destroy(g.pointer);
    g.pointer = nullptr;
  }
}
void seat_name(void *, wl_seat *, const char *) {}
const wl_seat_listener kSeat = {seat_caps, seat_name};

// our data device sees the same drags and selections as FLTK's: let them be
void d_offer(void *, wl_data_device *, wl_data_offer *o) { g.pending = o; }
void d_enter(void *, wl_data_device *, uint32_t, wl_surface *, wl_fixed_t, wl_fixed_t, wl_data_offer *o) {
  // a new drag: the earlier ones are over
  for (auto *old : g.drag_offers)
    if (old != o) wl_data_offer_destroy(old);
  g.drag_offers.clear();
  if (o) g.drag_offers.push_back(o);
  if (g.pending == o) g.pending = nullptr;
}
void d_leave(void *, wl_data_device *) {}
void d_motion(void *, wl_data_device *, uint32_t, wl_fixed_t, wl_fixed_t) {}
void d_drop(void *, wl_data_device *) {}
void d_selection(void *, wl_data_device *, wl_data_offer *o) {
  if (g.selection && g.selection != o) wl_data_offer_destroy(g.selection);
  g.selection = o;
  if (g.pending == o) g.pending = nullptr;
}
const wl_data_device_listener kDevice = {d_offer, d_enter, d_leave, d_motion, d_drop, d_selection};

void reg_global(void *, wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
  if (!strcmp(iface, wl_seat_interface.name) && !g.seat) {
    g.seat = (wl_seat *)wl_registry_bind(r, name, &wl_seat_interface, std::min<uint32_t>(ver, 5));
    wl_seat_add_listener(g.seat, &kSeat, nullptr);
  } else if (!strcmp(iface, wl_data_device_manager_interface.name) && !g.mgr) {
    g.mgr_ver = std::min<uint32_t>(ver, 3);
    g.mgr = (wl_data_device_manager *)wl_registry_bind(r, name, &wl_data_device_manager_interface, g.mgr_ver);
  } else if (!strcmp(iface, wl_shm_interface.name) && !g.shm) {
    g.shm = (wl_shm *)wl_registry_bind(r, name, &wl_shm_interface, 1);
  }
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kRegistry = {reg_global, reg_remove};

// one drag in progress
struct Drag {
  std::string text;
  bool uri_list = false;
  std::function<void(DragEnd)> done;
  wl_event_queue *queue = nullptr;
  wl_data_source *source = nullptr;
  wl_surface *icon = nullptr;
  std::vector<std::pair<wl_buffer *, std::pair<void *, size_t>>> buffers;
  DragImage image;
  int scale = 1;
  bool copy_badge = false;
  std::thread thread;
  int stop_pipe[2] = {-1, -1};
  std::atomic<bool> stopping{false};
  std::atomic<int> result{-1}; // DragEnd once known
} *g_drag = nullptr;

void src_target(void *, wl_data_source *, const char *) {}
void src_send(void *d, wl_data_source *, const char *, int32_t fd) {
  Drag *dr = (Drag *)d;
  // FLTK reads a drop on its own window from the main thread: write it all
  int fl = fcntl(fd, F_GETFL);
  if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
  const char *p = dr->text.data();
  size_t n = dr->text.size();
  while (n > 0) {
    ssize_t w = write(fd, p, n);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) break;
    p += w;
    n -= w;
  }
  close(fd);
}
void finish_in_main(void *);
void src_end(Drag *dr, DragEnd e) {
  int expected = -1;
  if (dr->result.compare_exchange_strong(expected, (int)e)) Fl::awake(finish_in_main, dr);
}
void src_cancelled(void *d, wl_data_source *) { src_end((Drag *)d, DragEnd::Cancelled); }
void src_drop_performed(void *, wl_data_source *) {}
void src_finished(void *d, wl_data_source *) { src_end((Drag *)d, DragEnd::Dropped); }
void src_action(void *, wl_data_source *, uint32_t) {}
const wl_data_source_listener kSource = {src_target, src_send, src_cancelled,
                                         src_drop_performed, src_finished, src_action};

// serves the drag source's events while the main thread may be blocked
void source_thread(Drag *dr) {
  wl_display *dpy = g.dpy;
  while (!dr->stopping) {
    while (wl_display_prepare_read_queue(dpy, dr->queue) != 0) wl_display_dispatch_queue_pending(dpy, dr->queue);
    wl_display_flush(dpy);
    pollfd fds[2] = {{wl_display_get_fd(dpy), POLLIN, 0}, {dr->stop_pipe[0], POLLIN, 0}};
    int r = poll(fds, 2, -1);
    if (r > 0 && (fds[0].revents & POLLIN) && !dr->stopping) {
      wl_display_read_events(dpy);
      Fl::awake(); // events for FLTK's queue may have been read here
    } else wl_display_cancel_read(dpy);
    wl_display_dispatch_queue_pending(dpy, dr->queue);
  }
}

wl_buffer *make_buffer(const DragPixels &px, std::pair<void *, size_t> &map) {
  size_t size = (size_t)px.w * px.h * 4;
  if (!g.shm || size == 0) return nullptr;
  int fd = memfd_create("fld-drag", MFD_CLOEXEC);
  if (fd < 0) return nullptr;
  if (ftruncate(fd, size) != 0) { close(fd); return nullptr; }
  void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mem == MAP_FAILED) { close(fd); return nullptr; }
  memcpy(mem, px.argb.data(), size);
  wl_shm_pool *pool = wl_shm_create_pool(g.shm, fd, size);
  wl_buffer *b = wl_shm_pool_create_buffer(pool, 0, px.w, px.h, px.w * 4, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  map = {mem, size};
  return b;
}

void show_image(Drag *dr, bool first) {
  if (!dr->icon) return;
  DragPixels px = render_drag_image(dr->image, dr->scale, dr->copy_badge);
  std::pair<void *, size_t> map{nullptr, 0};
  wl_buffer *b = make_buffer(px, map);
  if (!b) return;
  dr->buffers.push_back({b, map});
  int dx = first ? -px.hot_x : 0, dy = first ? -px.hot_y : 0;
  if (wl_proxy_get_version((wl_proxy *)dr->icon) >= 5) {
    wl_surface_attach(dr->icon, b, 0, 0);
#ifdef WL_SURFACE_OFFSET_SINCE_VERSION
    if (first) wl_surface_offset(dr->icon, dx, dy);
#endif
  } else wl_surface_attach(dr->icon, b, dx, dy);
  wl_surface_set_buffer_scale(dr->icon, dr->scale);
  wl_surface_damage(dr->icon, 0, 0, px.w, px.h);
  wl_surface_commit(dr->icon);
  wl_display_flush(g.dpy);
}

// a "+" while Ctrl is held: copying
void watch_keys(void *d) {
  Drag *dr = (Drag *)d;
  if (dr != g_drag) return;
  bool c = (Fl::event_state() & FL_CTRL) != 0;
  if (c != dr->copy_badge) {
    dr->copy_badge = c;
    show_image(dr, false);
  }
  Fl::repeat_timeout(0.06, watch_keys, d);
}

void finish_in_main(void *d) {
  Drag *dr = (Drag *)d;
  if (dr != g_drag) return;
  g_drag = nullptr;
  Fl::remove_timeout(watch_keys, dr);
  dr->stopping = true;
  if (write(dr->stop_pipe[1], "x", 1) < 0) {}
  if (dr->thread.joinable()) dr->thread.join();
  close(dr->stop_pipe[0]);
  close(dr->stop_pipe[1]);
  if (dr->source) wl_data_source_destroy(dr->source);
  if (dr->icon) wl_surface_destroy(dr->icon);
  for (auto &b : dr->buffers) {
    wl_buffer_destroy(b.first);
    munmap(b.second.first, b.second.second);
  }
  if (dr->queue) wl_event_queue_destroy(dr->queue);
  wl_display_flush(g.dpy);
  Fl::pushed(nullptr);
  DragEnd e = (DragEnd)dr->result.load();
  auto done = std::move(dr->done);
  delete dr;
  if (done) done(e);
}

} // namespace
#endif // FLD_DRAG_WAYLAND

void drag_setup() {
#ifdef FLD_DRAG_WAYLAND
  if (g.tried) return;
  wl_display *dpy = fl_wl_display();
  if (!dpy) return;
  g.tried = true;
  g.dpy = dpy;
  g.reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(g.reg, &kRegistry, nullptr);
  wl_display_roundtrip(dpy); // the globals
  wl_display_roundtrip(dpy); // the seat's capabilities
  if (g.seat && g.mgr) {
    g.device = wl_data_device_manager_get_data_device(g.mgr, g.seat);
    wl_data_device_add_listener(g.device, &kDevice, nullptr);
  }
#endif
}

static void setup_cb(void *) {
#ifdef FLD_DRAG_WAYLAND
  if (fl_wl_display()) drag_setup();
  else if (Fl::first_window()) return; // X11: Fl::dnd() is used
  else Fl::repeat_timeout(0.2, setup_cb); // the display opens with the first window
#endif
}

void drag_setup_later() {
#ifdef FLD_DRAG_WAYLAND
  static bool scheduled = false;
  if (scheduled || g.tried) return;
  scheduled = true;
  Fl::add_timeout(0.0, setup_cb);
#endif
}

bool drag_active() {
#ifdef FLD_DRAG_WAYLAND
  return g_drag != nullptr;
#else
  return false;
#endif
}

bool start_drag(Fl_Window *origin, const std::string &text, bool uri_list, const DragImage &img,
                std::function<void(DragEnd)> done) {
  if (!origin) return false;
  Fl_Window *top = origin->top_window();
#ifdef FLD_DRAG_WAYLAND
  if (fl_wl_display()) {
    drag_setup();
    if (g_drag || !g.device || !g.mgr || !top->shown() || g.press_serial == 0 || g.buttons <= 0) return false;
    wl_surface *surf = fl_wl_surface(fl_wl_xid(top));
    if (!surf) return false;
    Drag *dr = new Drag;
    dr->text = text;
    dr->uri_list = uri_list;
    dr->done = std::move(done);
    dr->image = img;
    dr->scale = std::max(1, fl_wl_buffer_scale(top));
    if (pipe2(dr->stop_pipe, O_CLOEXEC) != 0) { delete dr; return false; }
    dr->queue = wl_display_create_queue(g.dpy);
    wl_data_device_manager *mgr = (wl_data_device_manager *)wl_proxy_create_wrapper(g.mgr);
    wl_proxy_set_queue((wl_proxy *)mgr, dr->queue);
    dr->source = wl_data_device_manager_create_data_source(mgr);
    wl_proxy_wrapper_destroy(mgr);
    wl_data_source_add_listener(dr->source, &kSource, dr);
    // FLTK takes the first type it knows: uri-list first, so FLTK windows get paths
    if (uri_list) wl_data_source_offer(dr->source, "text/uri-list");
    wl_data_source_offer(dr->source, "text/plain;charset=utf-8");
    wl_data_source_offer(dr->source, "text/plain");
    wl_data_source_offer(dr->source, "UTF8_STRING");
    if (g.mgr_ver >= 3)
      wl_data_source_set_actions(dr->source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                                 WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
    dr->icon = wl_compositor_create_surface(fl_wl_compositor());
    wl_data_device_start_drag(g.device, dr->source, surf, dr->icon, g.press_serial);
    show_image(dr, true);
    g_drag = dr;
    g.buttons = 0; // the release goes to the drag
    // FLTK no longer sees this press: let go of the widget that had it
    Fl::pushed(nullptr);
    dr->thread = std::thread(source_thread, dr);
    Fl::add_timeout(0.06, watch_keys, dr);
    return true;
  }
#endif
  // X11 and other back ends: FLTK's own drag (it blocks until the drop there)
  Fl::copy(text.c_str(), (int)text.size(), 0);
  Fl::dnd();
  struct Later { std::function<void(DragEnd)> f; };
  Fl::add_timeout(0.0, [](void *d) {
    std::unique_ptr<Later> l((Later *)d);
    if (l->f) l->f(DragEnd::Dropped);
  }, new Later{std::move(done)});
  return true;
}

} // namespace fld
