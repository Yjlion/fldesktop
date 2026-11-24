// Whether a window is on screen, from Wayland frame callbacks
#pragma once

class Fl_Window;
struct wl_callback;

namespace fld {

// The compositor answers a surface's frame callback only while the surface is
// shown on an output; a minimised window, or one on another workspace, gets
// no answers. FrameWatch asks for one now and then, so a program with timers
// can skip work nobody would see.
//
// visible() is true until a request has gone unanswered for `grace` seconds
// (and on X11, where there are no such callbacks).
class FrameWatch {
public:
  explicit FrameWatch(Fl_Window *win, double grace = 1.5);
  ~FrameWatch();
  FrameWatch(const FrameWatch &) = delete;
  FrameWatch &operator=(const FrameWatch &) = delete;

  bool visible();
  // Called when visible() turns true again after it was false (the window
  // came back): timers that stopped can start again.
  void on_shown(void (*cb)(void *), void *data) {
    shown_cb_ = cb;
    shown_data_ = data;
  }

  // the wl_callback listener (internal)
  static void done(void *data, struct wl_callback *cb, unsigned int time);

private:
  void request();
  Fl_Window *win_;
  double grace_;
  struct wl_callback *pending_ = nullptr;
  double asked_ = 0; // when the pending request was made
  bool visible_ = true;
  void (*shown_cb_)(void *) = nullptr;
  void *shown_data_ = nullptr;
};

} // namespace fld
