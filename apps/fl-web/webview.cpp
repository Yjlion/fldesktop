#include "webview.h"

#include <time.h>

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include "fld/theme.h"
#include "glib_loop.h"

namespace web {

WebKitWebContext *g_web_context = nullptr;

namespace {

uint32_t now_ms() { return (uint32_t)(g_get_monotonic_time() / 1000); }

Fl_Cursor css_cursor(const std::string &n) {
  static const std::map<std::string, Fl_Cursor> m = {
      {"pointer", FL_CURSOR_HAND},      {"text", FL_CURSOR_INSERT},     {"vertical-text", FL_CURSOR_INSERT},
      {"wait", FL_CURSOR_WAIT},         {"progress", FL_CURSOR_WAIT},   {"crosshair", FL_CURSOR_CROSS},
      {"move", FL_CURSOR_MOVE},         {"all-scroll", FL_CURSOR_MOVE}, {"grab", FL_CURSOR_MOVE},
      {"grabbing", FL_CURSOR_MOVE},     {"ew-resize", FL_CURSOR_WE},    {"col-resize", FL_CURSOR_WE},
      {"e-resize", FL_CURSOR_E},        {"w-resize", FL_CURSOR_W},      {"ns-resize", FL_CURSOR_NS},
      {"row-resize", FL_CURSOR_NS},     {"n-resize", FL_CURSOR_N},      {"s-resize", FL_CURSOR_S},
      {"nesw-resize", FL_CURSOR_NESW},  {"ne-resize", FL_CURSOR_NE},    {"sw-resize", FL_CURSOR_SW},
      {"nwse-resize", FL_CURSOR_NWSE},  {"nw-resize", FL_CURSOR_NW},    {"se-resize", FL_CURSOR_SE},
      {"help", FL_CURSOR_HELP},         {"none", FL_CURSOR_NONE},       {"not-allowed", FL_CURSOR_DEFAULT},
  };
  auto it = m.find(n);
  return it == m.end() ? FL_CURSOR_DEFAULT : it->second;
}

// keysym for an FLTK key event: the typed character when there is one,
// else FLTK's key number (FLTK uses the X keysyms for the special keys)
unsigned keysym_for_event() {
  int k = Fl::event_key();
  const char *t = Fl::event_text();
  int len = Fl::event_length();
  if (len > 0 && (unsigned char)t[0] >= 0x20 && t[0] != 0x7f && !(k >= FL_KP && k <= FL_KP_Last && k != FL_KP + '.')) {
    unsigned cp = fl_utf8decode(t, t + len, nullptr);
    if (cp < 0x100) return cp;
    return 0x01000000u | cp;
  }
  if (k >= 'a' && k <= 'z' && (Fl::event_state() & FL_SHIFT)) return (unsigned)(k - 'a' + 'A');
  return (unsigned)k;
}

} // namespace

WebView::WebView(int x, int y, int w, int h, WebKitWebView *related, WebKitSettings *settings,
                 WebKitNetworkSession *session, WebKitUserContentManager *ucm)
    : Fl_Widget(x, y, w, h) {
  backend_ = Backend::create();
  if (wl_present::available()) {
    presenter_ = std::make_unique<wl_present::Presenter>();
    backend_->direct = true;
    backend_->on_dmabuf = [this](const void *key, const Dmabuf &buf, const std::vector<Rect> &damage, bool all,
                                 std::function<void()> shown, std::function<void()> release) {
      Fl_Window *win = window();
      if (!win || !win->shown()) return false;
      presenter_->place(win, this->x(), this->y(), this->w(), this->h());
      presenter_->set_mapped(visible_r());
      if (!presenter_->show(key, buf, damage, all, std::move(shown), std::move(release))) return false;
      if (!direct_shown_) {
        direct_shown_ = true;
        frame_.reset(); // nothing to draw here any more
        redraw();
      }
      return true;
    };
  }
  backend_->on_frame = [this](std::shared_ptr<const Frame> f) {
    if (direct_shown_) {
      // back to frames read into memory (WebKit rendered with the CPU)
      direct_shown_ = false;
      if (presenter_) presenter_->set_mapped(false);
      convert_all_ = true;
    }
    frame_ = f;
    if (f->all) {
      convert_all_ = true;
      changed_.clear();
      frame_pending_ = true;
      redraw();
      return;
    }
    // only where the page changed
    for (const Rect &r : f->damage) {
      if (!convert_all_) changed_.push_back(r);
      damage(FL_DAMAGE_USER1, this->x() + r.x, this->y() + r.y, r.w, r.h);
    }
    if (changed_.size() > 16) {
      // many frames before a draw (a hidden tab): convert it all once
      convert_all_ = true;
      changed_.clear();
    }
    if (!f->damage.empty() && visible_r()) {
      frame_pending_ = true;
    } else if (!frame_pending_) {
      // nothing to show: WebKit may go on at once
      schedule_frame_done();
    }
  };
  backend_->on_cursor = [this](const std::string &c) {
    if (window() && active_ && Fl::belowmouse() == this) window()->cursor(css_cursor(c));
  };
  view_ = backend_->create_view(related, settings, session, ucm);
  g_object_ref_sink(view_);
  backend_->resize(w, h);
}

WebView::~WebView() {
  Fl::remove_timeout(frame_done_cb, this);
  // first: it gives WebKit its buffers back while the view still exists
  presenter_.reset();
  if (view_) g_object_unref(view_);
  backend_.reset();
}

void WebView::update_presenter() {
  if (!presenter_) return;
  bool on = visible_r() && direct_shown_;
  presenter_->set_mapped(on);
  if (on && window() && window()->shown()) presenter_->place(window(), x(), y(), w(), h());
}

void WebView::show() {
  Fl_Widget::show();
  update_presenter();
}

void WebView::hide() {
  Fl_Widget::hide();
  update_presenter();
}

void WebView::activate_view(bool on) {
  active_ = on;
  backend_->set_visible(on && !hidden_);
  if (!on) backend_->set_focus(false);
}

void WebView::resize(int X, int Y, int W, int H) {
  bool sized = W != w() || H != h();
  Fl_Widget::resize(X, Y, W, H);
  if (sized) backend_->resize(W, H);
  if (presenter_ && direct_shown_ && visible_r() && window() && window()->shown())
    presenter_->place(window(), X, Y, W, H);
}

static double mono_now() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

void WebView::schedule_frame_done() {
  if (done_scheduled_) return;
  done_scheduled_ = true;
  double delay = std::max(0.0, last_done_ + 1.0 / 60 - mono_now());
  Fl_Window *top = top_window();
  if (top != watched_) {
    watched_ = top;
    watch_.reset(top ? new fld::FrameWatch(top) : nullptr);
    if (watch_) watch_->on_shown(window_shown_cb, this);
  }
  bool hidden = watch_ && !watch_->visible();
  if (hidden != hidden_) {
    hidden_ = hidden;
    // WebKit stops animations and timers of pages that are not visible
    if (active_) backend_->set_visible(!hidden);
  }
  if (hidden) delay = std::max(delay, 0.5);
  Fl::add_timeout(delay, frame_done_cb, this);
}

void WebView::window_shown_cb(void *d) {
  WebView *v = (WebView *)d;
  if (!v->hidden_) return;
  v->hidden_ = false;
  if (v->active_) v->backend_->set_visible(true);
  v->redraw();
}

void WebView::frame_done_cb(void *d) {
  WebView *v = (WebView *)d;
  v->done_scheduled_ = false;
  v->last_done_ = mono_now();
  v->backend_->frame_shown();
  glib_loop_poke();
}

// premultiplied BGRA -> RGB (pages are opaque), r within rgb_w_ x rgb_h_
void WebView::convert(const Rect &r) {
  const Frame *f = frame_.get();
  for (int row = r.y; row < r.y + r.h; row++) {
    const uint8_t *s = f->pixels.data() + (size_t)row * f->stride + (size_t)r.x * 4;
    unsigned char *d = rgb_.data() + ((size_t)row * rgb_w_ + r.x) * 3;
    for (int col = 0; col < r.w; col++, s += 4, d += 3) {
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
    }
  }
}

void WebView::draw() {
  if (direct_shown_ && presenter_) {
    // the page is a subsurface over this: only its place may have changed
    update_presenter();
    return;
  }
  fl_push_clip(x(), y(), w(), h());
  const Frame *f = frame_.get();
  if (!f || f->pixels.empty()) {
    fl_color(FL_WHITE);
    fl_rectf(x(), y(), w(), h());
  } else {
    int fw = std::min(f->width, w()), fh = std::min(f->height, h());
    if (fw != rgb_w_ || fh != rgb_h_) {
      rgb_w_ = fw;
      rgb_h_ = fh;
      rgb_.assign((size_t)fw * fh * 3, 0);
      convert_all_ = true;
    }
    // the parts that changed, within what is shown
    std::vector<Rect> parts;
    for (const Rect &r : changed_) {
      int x1 = std::min(r.x + r.w, fw), y1 = std::min(r.y + r.h, fh);
      if (x1 > r.x && y1 > r.y) parts.push_back({r.x, r.y, x1 - r.x, y1 - r.y});
    }
    if (convert_all_) convert({0, 0, fw, fh});
    else
      for (const Rect &r : parts) convert(r);
    if (damage() == FL_DAMAGE_USER1 && !convert_all_) {
      for (const Rect &r : parts)
        fl_draw_image(rgb_.data() + ((size_t)r.y * fw + r.x) * 3, x() + r.x, y() + r.y, r.w, r.h, 3, fw * 3);
    } else {
      fl_draw_image(rgb_.data(), x(), y(), fw, fh, 3);
      fl_color(FL_WHITE);
      if (fw < w()) fl_rectf(x() + fw, y(), w() - fw, h());
      if (fh < h()) fl_rectf(x(), y() + fh, fw, h() - fh);
    }
    convert_all_ = false;
    changed_.clear();
  }
  fl_pop_clip();
  if (frame_pending_) {
    frame_pending_ = false;
    // let WebKit render the next frame once this one is on screen
    schedule_frame_done();
  }
}

unsigned WebView::mods() const {
  int s = Fl::event_state();
  unsigned m = 0;
  if (s & FL_CTRL) m |= MOD_CTRL;
  if (s & FL_SHIFT) m |= MOD_SHIFT;
  if (s & FL_ALT) m |= MOD_ALT;
  if (s & FL_META) m |= MOD_META;
  if (s & FL_BUTTON1) m |= MOD_BUTTON1;
  if (s & FL_BUTTON2) m |= MOD_BUTTON2;
  if (s & FL_BUTTON3) m |= MOD_BUTTON3;
  return m;
}

void WebView::send_key(bool down) {
  backend_->key(down, keysym_for_event(), 0, mods(), now_ms());
  glib_loop_poke();
}

bool WebView::natural_scroll = false;

void WebView::paste_clipboard() {
  if (backend_->shares_clipboard()) {
    paste_key_ = 'v';
    Fl::paste(*this, 1);
    return;
  }
  backend_->key(true, 'v', 0, MOD_CTRL, now_ms());
  backend_->key(false, 'v', 0, MOD_CTRL, now_ms());
  glib_loop_poke();
}

int WebView::handle(int e) {
  double ex = Fl::event_x() - x(), ey = Fl::event_y() - y();
  switch (e) {
    case FL_FOCUS:
      backend_->set_focus(true);
      glib_loop_poke();
      return 1;
    case FL_UNFOCUS:
      backend_->set_focus(false);
      glib_loop_poke();
      return 1;
    case FL_ENTER:
      return 1;
    case FL_LEAVE:
      backend_->pointer_leave(ex, ey, mods(), now_ms());
      if (window()) window()->cursor(FL_CURSOR_DEFAULT);
      glib_loop_poke();
      return 1;
    case FL_MOVE: case FL_DRAG:
      backend_->pointer_motion(ex, ey, mods(), now_ms());
      glib_loop_poke();
      return 1;
    case FL_PUSH: case FL_RELEASE: {
      if (e == FL_PUSH && Fl::focus() != this) take_focus();
      int b = Fl::event_button();
      if (b < 1 || b > 3) return 1;
      backend_->pointer_button(e == FL_PUSH, (unsigned)b, ex, ey, mods(), (unsigned)Fl::event_clicks() + 1, now_ms());
      glib_loop_poke();
      if (e == FL_RELEASE && b == 3 && on_right_click) on_right_click(Fl::event_x(), Fl::event_y());
      return 1;
    }
    case FL_MOUSEWHEEL: {
      // FLTK gives steps (or pixels from touchpads on some platforms): 3 lines
      double dx = Fl::event_dx() * 48.0, dy = Fl::event_dy() * 48.0;
      if (natural_scroll) {
        dx = -dx;
        dy = -dy;
      }
      backend_->scroll(dx, dy, ex, ey, mods(), now_ms());
      glib_loop_poke();
      return 1;
    }
    case FL_SHORTCUT:
      return 0;
    case FL_KEYBOARD: {
      paste_key_ = 0;
      if (key_filter && key_filter(Fl::event_key(), Fl::event_state())) return 1;
      int k = Fl::event_key();
      bool ctrl = Fl::event_state() & FL_CTRL;
      if (backend_->shares_clipboard() && ((ctrl && (k == 'v' || k == 'V')) || ((Fl::event_state() & FL_SHIFT) && k == FL_Insert))) {
        // fetch the system clipboard first, then send the key
        paste_key_ = k;
        Fl::paste(*this, 1);
        return 1;
      }
      send_key(true);
      return 1;
    }
    case FL_KEYUP:
      if (paste_key_ && Fl::event_key() == paste_key_) return 1; // sent with the paste
      send_key(false);
      return 1;
    case FL_PASTE: {
      if (!paste_key_) return 1;
      backend_->set_clipboard_text(std::string(Fl::event_text(), Fl::event_length()));
      unsigned m = MOD_CTRL;
      unsigned sym = paste_key_ == FL_Insert ? (unsigned)FL_Insert : (unsigned)'v';
      if (paste_key_ == FL_Insert) m = MOD_SHIFT;
      paste_key_ = 0;
      backend_->key(true, sym, 0, m, now_ms());
      backend_->key(false, sym, 0, m, now_ms());
      glib_loop_poke();
      return 1;
    }
  }
  return Fl_Widget::handle(e);
}

} // namespace web
