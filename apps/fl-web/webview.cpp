#include "webview.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <cstring>
#include <map>
#include <string>

#include "fld/theme.h"
#include "glib_loop.h"

namespace web {

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
  backend_->on_frame = [this](std::shared_ptr<const Frame> f) {
    frame_ = f;
    frame_pending_ = true;
    redraw();
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
  if (view_) g_object_unref(view_);
  backend_.reset();
}

void WebView::activate_view(bool on) {
  active_ = on;
  backend_->set_visible(on);
  if (!on) backend_->set_focus(false);
}

void WebView::resize(int X, int Y, int W, int H) {
  bool sized = W != w() || H != h();
  Fl_Widget::resize(X, Y, W, H);
  if (sized) backend_->resize(W, H);
}

void WebView::frame_done_cb(void *d) {
  WebView *v = (WebView *)d;
  v->backend_->frame_shown();
  glib_loop_poke();
}

void WebView::draw() {
  fl_push_clip(x(), y(), w(), h());
  const Frame *f = frame_.get();
  if (!f || f->pixels.empty()) {
    fl_color(FL_WHITE);
    fl_rectf(x(), y(), w(), h());
  } else {
    int fw = std::min(f->width, w()), fh = std::min(f->height, h());
    // premultiplied BGRA -> RGB (pages are opaque)
    rgb_.resize((size_t)fw * fh * 3);
    for (int row = 0; row < fh; row++) {
      const uint8_t *s = f->pixels.data() + (size_t)row * f->stride;
      unsigned char *d = rgb_.data() + (size_t)row * fw * 3;
      for (int col = 0; col < fw; col++, s += 4, d += 3) {
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
      }
    }
    fl_draw_image(rgb_.data(), x(), y(), fw, fh, 3);
    fl_color(FL_WHITE);
    if (fw < w()) fl_rectf(x() + fw, y(), w() - fw, h());
    if (fh < h()) fl_rectf(x(), y() + fh, fw, h() - fh);
  }
  fl_pop_clip();
  if (frame_pending_) {
    frame_pending_ = false;
    // let WebKit render the next frame once this one is on screen
    Fl::add_timeout(0, frame_done_cb, this);
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
      return 1;
    }
    case FL_MOUSEWHEEL: {
      // FLTK gives steps (or pixels from touchpads on some platforms): 3 lines
      double dx = Fl::event_dx() * 48.0, dy = Fl::event_dy() * 48.0;
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
