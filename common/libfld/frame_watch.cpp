#include "fld/frame_watch.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <time.h>

#if defined(FLTK_USE_WAYLAND)
#include <wayland-client.h>
#endif

namespace fld {

static double now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

FrameWatch::FrameWatch(Fl_Window *win, double grace) : win_(win), grace_(grace) {}

FrameWatch::~FrameWatch() {
#if defined(FLTK_USE_WAYLAND)
  if (pending_) wl_callback_destroy(pending_);
#endif
}

#if defined(FLTK_USE_WAYLAND)
static const wl_callback_listener kListener = {
    [](void *data, wl_callback *cb, uint32_t time) { FrameWatch::done(data, cb, time); }};
#endif

void FrameWatch::done(void *data, struct wl_callback *cb, unsigned int) {
#if defined(FLTK_USE_WAYLAND)
  FrameWatch *w = (FrameWatch *)data;
  wl_callback_destroy(cb);
  if (w->pending_ == cb) w->pending_ = nullptr;
  if (!w->visible_) {
    w->visible_ = true;
    if (w->shown_cb_) w->shown_cb_(w->shown_data_);
  }
#else
  (void)data;
  (void)cb;
#endif
}

void FrameWatch::request() {
#if defined(FLTK_USE_WAYLAND)
  if (!fl_wl_display() || !win_ || !win_->shown()) return;
  wld_window *xid = fl_wl_xid(win_);
  wl_surface *s = xid ? fl_wl_surface(xid) : nullptr;
  if (!s) return;
  pending_ = wl_surface_frame(s);
  wl_callback_add_listener(pending_, &kListener, this);
  // an empty commit: the callback is sent with the next frame the surface is
  // shown in; nothing else of the surface's state changes
  wl_surface_commit(s);
  wl_display_flush(fl_wl_display());
  asked_ = now_s();
#endif
}

bool FrameWatch::visible() {
#if defined(FLTK_USE_WAYLAND)
  if (!fl_wl_display()) return true;
  if (!win_ || !win_->shown() || !win_->visible()) {
    visible_ = false;
    return false;
  }
  if (!pending_) request();
  else if (now_s() - asked_ > grace_) visible_ = false;
  // a window that has not got its callback yet counts as shown for `grace`
  return visible_;
#else
  return true;
#endif
}

} // namespace fld
