// An FLTK widget showing one WebKitWebView: draws its frames and sends it
// the pointer and keyboard input.
#pragma once
#include <FL/Fl_Widget.H>

#include <functional>
#include <memory>

#include "fld/frame_watch.h"

#include "backend.h"

namespace web {

class WebView : public Fl_Widget {
public:
  // related: the page that opened this one (window.open), or null
  WebView(int x, int y, int w, int h, WebKitWebView *related, WebKitSettings *settings,
          WebKitNetworkSession *session, WebKitUserContentManager *ucm);
  ~WebView() override;

  WebKitWebView *view() const { return view_; }
  // Keys the browser handles first (shortcuts); returns true when used.
  std::function<bool(int key, int state)> key_filter;
  // a right button release at x, y (window coordinates)
  std::function<void(int x, int y)> on_right_click;
  // wheel down scrolls up, like a touch screen
  static bool natural_scroll;
  // pastes the system clipboard into the page (as a Ctrl+V)
  void paste_clipboard();

  void activate_view(bool on); // shown in the window (the current tab)
  void draw() override;
  int handle(int e) override;
  void resize(int x, int y, int w, int h) override;

private:
  unsigned mods() const;
  void send_key(bool down);
  std::unique_ptr<Backend> backend_;
  WebKitWebView *view_ = nullptr;
  std::shared_ptr<const Frame> frame_;
  std::vector<unsigned char> rgb_; // frame_ as RGB, rgb_w_ x rgb_h_
  int rgb_w_ = 0, rgb_h_ = 0;
  bool convert_all_ = true;  // rgb_ is out of date
  std::vector<Rect> changed_; // parts of rgb_ out of date (and to draw)
  void convert(const Rect &r);
  bool active_ = false;
  bool frame_pending_ = false;
  int paste_key_ = 0; // Ctrl+V waiting for the clipboard
  static void frame_done_cb(void *);
  // lets WebKit render the next frame: at most at 60 Hz, and slowly while
  // the window is not on screen (minimised, another workspace)
  void schedule_frame_done();
  static void window_shown_cb(void *);
  bool done_scheduled_ = false, hidden_ = false;
  double last_done_ = 0;
  std::unique_ptr<fld::FrameWatch> watch_;
  Fl_Window *watched_ = nullptr;
};

} // namespace web
