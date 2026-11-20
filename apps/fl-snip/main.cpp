// fl-snip: screenshots, like a snipping tool
//
// The screen is captured first (grim) and shown frozen in an overlay over all
// outputs (fl-wm's "fldesktop.snip" role), where a rectangle, a freeform
// shape, a window or the whole screen is chosen. The snip then opens in an
// editor (pen, highlighter, shapes, text, pixelate, crop) or, for the quick
// modes the Print keys use, is saved and copied right away.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Copy_Surface.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/mime.h"
#include "fld/notify.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "snip_util.h"
#ifdef FLD_HAVE_RECORD
#include "record.h"
#endif

using snip::Image;
using snip::Point;
using snip::Rect;

static double now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ---------------------------------------------------------------------------
// Settings

enum class Mode { Rect, Freeform, Window, Full };

static struct Settings {
  bool copy = true;      // copy each snip to the clipboard
  bool autosave = true;  // save each snip to Pictures/Screenshots
  bool cursor = false;   // include the pointer
  bool notify = true;    // a notification after a quick snip
  Mode mode = Mode::Rect;
  int delay = 0;
  static std::string path() { return fld::fld_config_dir() + "/snip.ini"; }
  void load() {
    fld::Ini c;
    c.load(path());
    copy = c.get_bool("Snip", "CopyToClipboard", copy);
    autosave = c.get_bool("Snip", "AutoSave", autosave);
    cursor = c.get_bool("Snip", "IncludePointer", cursor);
    notify = c.get_bool("Snip", "Notify", notify);
    mode = (Mode)std::max(0, std::min(3, c.get_int("Snip", "Mode", 0)));
    delay = std::max(0, std::min(30, c.get_int("Snip", "Delay", 0)));
  }
  void save() const {
    fld::Ini c;
    c.load(path());
    c.set_bool("Snip", "CopyToClipboard", copy);
    c.set_bool("Snip", "AutoSave", autosave);
    c.set_bool("Snip", "IncludePointer", cursor);
    c.set_bool("Snip", "Notify", notify);
    c.set_int("Snip", "Mode", (int)mode);
    c.set_int("Snip", "Delay", delay);
    c.save(path());
  }
} S;

static const char *mode_name(Mode m) {
  switch (m) {
    case Mode::Rect: return "Rectangle";
    case Mode::Freeform: return "Freeform";
    case Mode::Window: return "Window";
    case Mode::Full: return "Full Screen";
  }
  return "";
}

static std::string screenshots_dir() {
  std::string pics = fld::user_dir("PICTURES");
  if (pics.empty()) pics = fld::home_dir() + "/Pictures";
  return pics + "/Screenshots";
}

// ---------------------------------------------------------------------------
// Capturing

struct Capture {
  Image img;
  int lx = 0, ly = 0, lw = 0, lh = 0; // the output layout (logical pixels)
  double sx = 1, sy = 1;              // layout to image pixels
};

static bool grab_screen(Capture &cap, std::string &err) {
  if (fld::which("grim").empty()) {
    err = "fl-snip needs grim to take screenshots.";
    return false;
  }
  fl_open_display();
  int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  for (int i = 0; i < Fl::screen_count(); i++) {
    int x, y, w, h;
    Fl::screen_xywh(x, y, w, h, i);
    if (i == 0 || x < x1) x1 = x;
    if (i == 0 || y < y1) y1 = y;
    if (i == 0 || x + w > x2) x2 = x + w;
    if (i == 0 || y + h > y2) y2 = y + h;
  }
  std::vector<std::string> cmd = {"grim", "-t", "ppm"};
  if (S.cursor) cmd.push_back("-c");
  cmd.push_back("-");
  std::string out;
  if (fld::run_capture(cmd, out) != 0 || !snip::parse_ppm(out, cap.img)) {
    err = "grim could not take a screenshot.";
    return false;
  }
  cap.lx = x1;
  cap.ly = y1;
  cap.lw = std::max(1, x2 - x1);
  cap.lh = std::max(1, y2 - y1);
  cap.sx = (double)cap.img.w / cap.lw;
  cap.sy = (double)cap.img.h / cap.lh;
  return true;
}

// fl-wm's windows, in layout coordinates (empty without fl-wm)
static std::vector<snip::Window> wm_windows() {
  const char *pid = getenv("FLWM_PID");
  if (!pid || atoi(pid) <= 1) return {};
  std::string path = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-wm-windows";
  struct stat before{};
  stat(path.c_str(), &before);
  if (kill(atoi(pid), SIGUSR2) != 0) return {};
  // fl-wm writes the file from its event loop
  for (int i = 0; i < 40; i++) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && (st.st_mtim.tv_sec != before.st_mtim.tv_sec ||
                                        st.st_mtim.tv_nsec != before.st_mtim.tv_nsec || before.st_ino == 0))
      break;
    usleep(10000);
  }
  std::string text;
  fld::read_file(path, text);
  return snip::parse_windows(text);
}

// Fl_RGB_Image over an Image's pixels (the Image must outlive it)
static std::unique_ptr<Fl_RGB_Image> rgb_image(const Image &img) {
  return std::unique_ptr<Fl_RGB_Image>(new Fl_RGB_Image(img.px.data(), img.w, img.h, 4));
}

// ---------------------------------------------------------------------------
// Small drawn glyphs for the tool and mode buttons (the icon theme has none)

enum class Glyph { Rect, Freeform, Window, Full, Pen, Highlighter, Line, Arrow, Box, Ellipse, Text, Pixelate,
                   Crop, Eraser };

static void draw_arrow_head(double x1, double y1, double x2, double y2, double len) {
  double a = atan2(y2 - y1, x2 - x1);
  double l1 = a + M_PI * 0.83, l2 = a - M_PI * 0.83;
  fl_begin_polygon();
  fl_vertex(x2, y2);
  fl_vertex(x2 + len * cos(l1), y2 + len * sin(l1));
  fl_vertex(x2 + len * cos(l2), y2 + len * sin(l2));
  fl_end_polygon();
}

static void draw_glyph(Glyph g, int x, int y, int s, Fl_Color c) {
  fl_color(c);
  fl_line_style(FL_SOLID, 2);
  int m = s / 5;
  int l = x + m, t = y + m, r = x + s - m, b = y + s - m;
  switch (g) {
    case Glyph::Rect:
      fl_line_style(FL_DASH, 2);
      fl_rect(l, t + 2, r - l, b - t - 4);
      break;
    case Glyph::Freeform:
      fl_line_style(FL_DASH, 2);
      fl_begin_loop();
      fl_vertex(l, (t + b) / 2);
      fl_vertex(l + (r - l) / 3, t);
      fl_vertex(r, t + 2);
      fl_vertex(r - 2, b);
      fl_vertex(l + (r - l) / 3, b - 3);
      fl_end_loop();
      break;
    case Glyph::Window:
      fl_rect(l, t, r - l, b - t);
      fl_rectf(l, t, r - l, 4);
      break;
    case Glyph::Full:
      fl_rect(l, t + 1, r - l, b - t - 5);
      fl_line(x + s / 2, b - 4, x + s / 2, b);
      fl_line(x + s / 2 - 4, b, x + s / 2 + 4, b);
      break;
    case Glyph::Pen:
      fl_line(l, b, r, t);
      fl_line(l, b, l + 3, b - 1);
      break;
    case Glyph::Highlighter:
      fl_line_style(FL_SOLID | FL_CAP_SQUARE, s / 4);
      fl_line(l + 2, b - 2, r - 2, t + 2);
      break;
    case Glyph::Line: fl_line(l, b, r, t); break;
    case Glyph::Arrow:
      fl_line(l, b, r - 2, t + 2);
      draw_arrow_head(l, b, r, t, s / 3.0);
      break;
    case Glyph::Box: fl_rect(l, t + 2, r - l, b - t - 4); break;
    case Glyph::Ellipse: fl_arc(l, t + 2, r - l, b - t - 4, 0, 360); break;
    case Glyph::Text:
      fl_font(FL_HELVETICA_BOLD, s - 4);
      fl_draw("T", x, y, s, s, FL_ALIGN_CENTER);
      break;
    case Glyph::Pixelate: {
      int q = (r - l) / 3;
      for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
          if ((i + j) % 2 == 0) fl_rectf(l + i * q, t + j * q, q, q);
      break;
    }
    case Glyph::Crop:
      fl_line(l + 3, t, l + 3, b - 3, r, b - 3);
      fl_line(l, t + 3, r - 3, t + 3, r - 3, b);
      break;
    case Glyph::Eraser:
      fl_begin_loop();
      fl_vertex(l, b - 4);
      fl_vertex(l + (r - l) / 2, t);
      fl_vertex(r, t + 5);
      fl_vertex(l + 5, b);
      fl_end_loop();
      fl_line(l, b, r, b);
      break;
  }
  fl_line_style(0);
}

class GlyphButton : public fld::FlatButton {
public:
  Glyph glyph;
  GlyphButton(int x, int y, int w, int h, Glyph g, const char *tip) : fld::FlatButton(x, y, w, h), glyph(g) {
    tooltip(tip);
  }

protected:
  void draw() override {
    fld::FlatButton::draw();
    int s = std::min(w(), h()) - 12;
    draw_glyph(glyph, x() + (w() - s) / 2, y() + (h() - s) / 2, s,
               active_r() ? (active_look() ? fld::palette().accent : fld::palette().fg) : fld::palette().fg_dim);
  }
};

// ---------------------------------------------------------------------------
// The selection overlay

class Overlay : public Fl_Double_Window {
public:
  Overlay(const Capture &cap, Mode mode)
      : Fl_Double_Window(0, 0, cap.lw, cap.lh, "fldesktop.snip"), cap_(cap), mode_(mode) {
    border(0);
    box(FL_NO_BOX);
    // the dimmed copy everything but the selection shows
    dim_ = cap.img;
    for (size_t i = 0; i < dim_.px.size(); i += 4)
      for (int c = 0; c < 3; c++) dim_.px[i + c] = (uint8_t)(dim_.px[i + c] * 0.5);
    bright_img_ = rgb_image(cap.img);
    dim_img_ = rgb_image(dim_);
    bright_img_->scale(w(), h(), 0, 1);
    dim_img_->scale(w(), h(), 0, 1);
    if (mode_ == Mode::Window) windows_ = wm_windows();
    make_bar();
    end();
    cursor(FL_CURSOR_CROSS);
  }

  // the chosen part in image pixels (empty: cancelled)
  Image result;
  bool done = false;

  int handle(int e) override {
    if (Fl_Double_Window::handle(e) && (e == FL_PUSH || e == FL_RELEASE) && Fl::event_inside(bar_)) return 1;
    int ex = Fl::event_x(), ey = Fl::event_y();
    switch (e) {
      case FL_FOCUS: case FL_UNFOCUS: return 1;
      case FL_KEYBOARD: return key(Fl::event_key());
      case FL_ENTER: case FL_MOVE:
        mx_ = ex;
        my_ = ey;
        if (mode_ == Mode::Window) {
          int i = snip::window_at(windows_, ex + cap_.lx, ey + cap_.ly);
          if (i != hover_win_) hover_win_ = i;
        }
        redraw();
        return 1;
      case FL_PUSH:
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          finish(false);
          return 1;
        }
        dragging_ = true;
        ax_ = ex;
        ay_ = ey;
        sel_ = snip::span(ex, ey, ex, ey);
        poly_.clear();
        poly_.push_back({ex, ey});
        redraw();
        return 1;
      case FL_DRAG:
        mx_ = ex;
        my_ = ey;
        if (!dragging_) return 1;
        if (mode_ == Mode::Freeform) {
          if (poly_.empty() || std::abs(poly_.back().first - ex) + std::abs(poly_.back().second - ey) > 2)
            poly_.push_back({ex, ey});
        } else {
          sel_ = snip::clip(snip::span(ax_, ay_, ex, ey), w(), h());
        }
        redraw();
        return 1;
      case FL_RELEASE:
        if (!dragging_) return 1;
        dragging_ = false;
        if (mode_ == Mode::Window) {
          if (hover_win_ >= 0) {
            Rect r = windows_[(size_t)hover_win_].r;
            r.x -= cap_.lx;
            r.y -= cap_.ly;
            sel_ = snip::clip(r, w(), h());
            finish(true);
          }
          return 1;
        }
        if (mode_ == Mode::Full) {
          sel_ = {0, 0, w(), h()};
          finish(true);
          return 1;
        }
        if (mode_ == Mode::Freeform) {
          if (poly_.size() >= 3) finish(true);
          return 1;
        }
        // a click without a drag: nothing chosen yet
        if (sel_.w > 3 && sel_.h > 3) finish(true);
        return 1;
    }
    return 1;
  }

  void draw() override {
    dim_img_->draw(0, 0);
    const fld::Palette &p = fld::palette();
    Rect lit;
    bool have = false;
    if (mode_ == Mode::Rect && (dragging_ || !sel_.empty()) && !sel_.empty()) {
      lit = sel_;
      have = true;
    } else if (mode_ == Mode::Window && hover_win_ >= 0) {
      lit = windows_[(size_t)hover_win_].r;
      lit.x -= cap_.lx;
      lit.y -= cap_.ly;
      lit = snip::clip(lit, w(), h());
      have = !lit.empty();
    } else if (mode_ == Mode::Full) {
      lit = {0, 0, w(), h()};
      have = true;
    }
    if (have) {
      fl_push_clip(lit.x, lit.y, lit.w, lit.h);
      bright_img_->draw(0, 0);
      fl_pop_clip();
      fl_color(p.accent);
      fl_line_style(FL_SOLID, 2);
      fl_rect(lit.x, lit.y, lit.w, lit.h);
      fl_line_style(0);
      size_label(lit);
    }
    if (mode_ == Mode::Freeform && poly_.size() > 1) {
      fl_color(p.accent);
      fl_line_style(FL_SOLID, 2);
      fl_begin_line();
      for (auto &pt : poly_) fl_vertex(pt.first, pt.second);
      fl_end_line();
      fl_line_style(0);
    }
    if ((mode_ == Mode::Rect || mode_ == Mode::Freeform) && !dragging_ && sel_.empty()) {
      // a crosshair through the pointer
      fl_color(fl_color_average(p.accent, FL_WHITE, 0.6f));
      fl_line_style(FL_DOT, 1);
      fl_xyline(0, my_, w());
      fl_yxline(mx_, 0, h());
      fl_line_style(0);
    }
    if (mode_ == Mode::Rect || mode_ == Mode::Freeform) magnifier();
    draw_children();
  }

private:
  const Capture &cap_;
  Mode mode_;
  Image dim_;
  std::unique_ptr<Fl_RGB_Image> bright_img_, dim_img_;
  std::vector<snip::Window> windows_;
  int hover_win_ = -1;
  bool dragging_ = false;
  int ax_ = 0, ay_ = 0, mx_ = -100, my_ = -100;
  Rect sel_;
  std::vector<Point> poly_;
  Fl_Group *bar_ = nullptr;
  std::vector<GlyphButton *> mode_btns_;

  void make_bar() {
    const int B = 40, n = 4, W = n * (B + 4) + 4 + 8 + 90;
    bar_ = new Fl_Group((w() - W) / 2, 16, W, B + 8);
    bar_->box(FL_ROUNDED_BOX);
    bar_->color(fld::palette().bg);
    static const struct { Mode m; Glyph g; const char *tip; } modes[] = {
        {Mode::Rect, Glyph::Rect, "Rectangle (R)"},
        {Mode::Freeform, Glyph::Freeform, "Freeform (F)"},
        {Mode::Window, Glyph::Window, "Window (W)"},
        {Mode::Full, Glyph::Full, "Full screen (S)"}};
    int x = bar_->x() + 4;
    for (auto &md : modes) {
      auto *b = new GlyphButton(x, bar_->y() + 4, B, B, md.g, md.tip);
      b->color(fld::palette().bg);
      b->active_look(md.m == mode_);
      b->callback([](Fl_Widget *w, void *d) {
        Overlay *o = (Overlay *)w->window();
        o->set_mode((Mode)(long)d);
      }, (void *)(long)md.m);
      mode_btns_.push_back(b);
      x += B + 4;
    }
    auto *cancel = new Fl_Button(x + 8, bar_->y() + 8, 84, B - 8, "Cancel");
    cancel->callback([](Fl_Widget *w, void *) { ((Overlay *)w->window())->finish(false); });
    cancel->tooltip("Esc");
    bar_->end();
  }

  void set_mode(Mode m) {
    mode_ = m;
    S.mode = m;
    sel_ = {};
    poly_.clear();
    hover_win_ = -1;
    if (m == Mode::Window && windows_.empty()) windows_ = wm_windows();
    for (size_t i = 0; i < mode_btns_.size(); i++) mode_btns_[i]->active_look((int)i == (int)m);
    redraw();
  }

  int key(int k) {
    switch (k) {
      case FL_Escape: finish(false); return 1;
      case 'r': set_mode(Mode::Rect); return 1;
      case 'f': set_mode(Mode::Freeform); return 1;
      case 'w': set_mode(Mode::Window); return 1;
      case 's': set_mode(Mode::Full); return 1;
      case FL_Enter: case FL_KP_Enter:
        if (mode_ == Mode::Full) {
          sel_ = {0, 0, w(), h()};
          finish(true);
        } else if (mode_ == Mode::Rect && !sel_.empty()) {
          finish(true);
        }
        return 1;
      case FL_Left: case FL_Right: case FL_Up: case FL_Down: {
        // nudge the rectangle (Shift: its size)
        if (mode_ != Mode::Rect || sel_.empty()) return 1;
        int dx = k == FL_Left ? -1 : k == FL_Right ? 1 : 0, dy = k == FL_Up ? -1 : k == FL_Down ? 1 : 0;
        if (Fl::event_state() & FL_SHIFT) {
          sel_.w = std::max(1, sel_.w + dx);
          sel_.h = std::max(1, sel_.h + dy);
        } else {
          sel_.x += dx;
          sel_.y += dy;
        }
        sel_ = snip::clip(sel_, w(), h());
        redraw();
        return 1;
      }
    }
    return 0;
  }

  void size_label(const Rect &r) {
    Rect img = snip::scale(r, cap_.sx, cap_.sy);
    char buf[64];
    snprintf(buf, sizeof buf, "%d × %d", img.w, img.h);
    fl_font(FL_HELVETICA_BOLD, 13);
    int tw = (int)fl_width(buf) + 16, th = 24;
    int tx = r.x, ty = r.y - th - 6;
    if (ty < 0) ty = r.y + 6;
    fl_color(fld::palette().accent);
    fl_rounded_rectf(tx, ty, tw, th, 4);
    fl_color(fld::palette().accent_fg);
    fl_draw(buf, tx, ty, tw, th, FL_ALIGN_CENTER);
  }

  // an enlarged view of the pixels around the pointer
  void magnifier() {
    if (mx_ < 0 || my_ < 0 || mx_ >= w() || my_ >= h()) return;
    const int N = 15, Z = 8, S2 = N * Z;
    int ix = (int)(mx_ * cap_.sx) - N / 2, iy = (int)(my_ * cap_.sy) - N / 2;
    Image part = snip::crop(cap_.img, {std::max(0, ix), std::max(0, iy), N, N});
    if (part.empty()) return;
    Fl_RGB_Image pi(part.px.data(), part.w, part.h, 4);
    Fl_RGB_Image::RGB_scaling(FL_RGB_SCALING_NEAREST);
    std::unique_ptr<Fl_Image> big(pi.copy(part.w * Z, part.h * Z));
    Fl_RGB_Image::RGB_scaling(FL_RGB_SCALING_BILINEAR);
    int bx = mx_ + 24, by = my_ + 24;
    if (bx + S2 + 4 > w()) bx = mx_ - 24 - S2;
    if (by + S2 + 28 > h()) by = my_ - 24 - S2 - 24;
    fl_color(fld::palette().border);
    fl_rectf(bx - 2, by - 2, S2 + 4, S2 + 28);
    big->draw(bx, by);
    fl_color(fld::palette().accent);
    fl_rect(bx + (N / 2) * Z, by + (N / 2) * Z, Z, Z);
    char buf[48];
    snprintf(buf, sizeof buf, "%d, %d", (int)(mx_ * cap_.sx), (int)(my_ * cap_.sy));
    fl_color(fld::palette().fg);
    fl_font(FL_HELVETICA, 12);
    fl_draw(buf, bx, by + S2, S2, 24, FL_ALIGN_CENTER);
  }

  void finish(bool ok) {
    done = true;
    if (ok) {
      if (mode_ == Mode::Freeform) {
        Rect b = snip::clip(snip::bounds(poly_), w(), h());
        Rect ib = snip::scale(b, cap_.sx, cap_.sy);
        result = snip::crop(cap_.img, ib);
        std::vector<Point> rel;
        for (auto &pt : poly_)
          rel.push_back({(int)lround(pt.first * cap_.sx) - ib.x, (int)lround(pt.second * cap_.sy) - ib.y});
        snip::mask_outside(result, rel);
      } else {
        result = snip::crop(cap_.img, snip::scale(sel_, cap_.sx, cap_.sy));
      }
    }
    hide();
  }
};

// Captures the screen after the delay and lets the user choose; empty when
// cancelled. Windows of this program are hidden meanwhile.
static Image take_snip(Mode mode, int delay, std::string &err) {
  std::vector<Fl_Window *> hidden;
  for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w)) hidden.push_back(w);
  for (auto *w : hidden) w->hide();
  // let the compositor draw the screen without them
  double until = now_s() + std::max(0.35, (double)delay);
  while (now_s() < until) Fl::wait(0.05);
  Capture cap;
  Image result;
  if (grab_screen(cap, err)) {
    if (mode == Mode::Full) {
      result = cap.img;
    } else {
      Overlay o(cap, mode);
      o.show();
      o.take_focus();
      while (!o.done && o.shown()) Fl::wait();
      result = std::move(o.result);
    }
  }
  for (auto *w : hidden) w->show();
  return result;
}

// ---------------------------------------------------------------------------
// Saving and copying

static bool write_png(const Image &img, const std::string &path) {
  // drop the alpha channel when nothing is transparent
  bool alpha = false;
  for (size_t i = 3; i < img.px.size(); i += 4)
    if (img.px[i] != 255) {
      alpha = true;
      break;
    }
  if (alpha) return fl_write_png(path.c_str(), img.px.data(), img.w, img.h, 4) == 0;
  std::vector<uint8_t> rgb((size_t)img.w * img.h * 3);
  for (size_t i = 0, j = 0; i < img.px.size(); i += 4, j += 3) {
    rgb[j] = img.px[i];
    rgb[j + 1] = img.px[i + 1];
    rgb[j + 2] = img.px[i + 2];
  }
  return fl_write_png(path.c_str(), rgb.data(), img.w, img.h, 3) == 0;
}

static std::string auto_save(const Image &img) {
  std::string dir = screenshots_dir();
  fld::mkdir_p(dir);
  std::string name = snip::file_name(time(nullptr));
  std::string stem = name.substr(0, name.size() - 4); // without ".png"
  std::string path = dir + "/" + name;
  for (int i = 2; fld::file_exists(path); i++) path = dir + "/" + stem + "-" + std::to_string(i) + ".png";
  return write_png(img, path) ? path : "";
}

// wl-copy keeps the picture on the clipboard after fl-snip exits; FLTK's
// clipboard only lasts while it runs
static void copy_image(const Image &img, bool lasting) {
  if (lasting && !fld::which("wl-copy").empty()) {
    std::string tmp = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-snip-copy.png";
    if (write_png(img, tmp)) {
      fld::spawn_shell("wl-copy --type image/png < " + fld::shell_quote(tmp) + "; rm -f " + fld::shell_quote(tmp));
      return;
    }
  }
  std::vector<uint8_t> rgb((size_t)img.w * img.h * 3);
  for (size_t i = 0, j = 0; i < img.px.size(); i += 4, j += 3) {
    uint8_t a = img.px[i + 3];
    // transparent parts become white
    for (int c = 0; c < 3; c++) rgb[j + c] = (uint8_t)((img.px[i + c] * a + 255 * (255 - a)) / 255);
  }
  // what is drawn on a copy surface goes to the clipboard when it is deleted
  Fl_RGB_Image pic(rgb.data(), img.w, img.h, 3);
  auto *cs = new Fl_Copy_Surface(img.w, img.h);
  Fl_Surface_Device::push_current(cs);
  pic.draw(0, 0);
  Fl_Surface_Device::pop_current();
  delete cs;
}

// ---------------------------------------------------------------------------
// The editor

enum class Tool { Pen, Highlighter, Line, Arrow, Box, Ellipse, Text, Pixelate, Crop, Eraser };

struct Shape {
  Tool tool;
  Fl_Color color;
  int size;
  std::vector<Point> pts; // image pixels; two points for lines, boxes, crops
  std::string text;
};

static const Fl_Color kColors[] = {0xE01B2400, 0xF6D32D00, 0x33D17A00, 0x3584E400, 0x9141AC00, 0xFFFFFF00,
                                   0x00000000};
static const char *kColorNames[] = {"Red", "Yellow", "Green", "Blue", "Purple", "White", "Black"};

class EditorWindow;
static std::vector<EditorWindow *> g_editors;
static void new_snip(Mode mode, int delay);
static void show_settings();

class Canvas : public Fl_Widget {
public:
  EditorWindow *ed;
  double zoom = 1;
  bool fit = true;
  Canvas(int x, int y, int w, int h, EditorWindow *e) : Fl_Widget(x, y, w, h), ed(e) {}
  void draw() override;
  int handle(int e) override;
  // image pixel <-> widget coordinates
  double ox() const;
  double oy() const;
  Point to_image(int x, int y) const;
};

class EditorWindow : public Fl_Double_Window {
public:
  Image base;      // the snip as taken
  Image work;      // base with the pixel shapes (highlighter, pixelate) applied
  std::vector<Shape> shapes;
  std::vector<std::vector<Shape>> undo_stack, redo_stack;
  std::unique_ptr<Fl_RGB_Image> work_img;
  std::string path; // where it was saved last
  bool dirty = false;
  Tool tool = Tool::Pen;
  int color = 0, pen_size = 4;
  Shape *live = nullptr; // being drawn
  Shape live_shape;
  Canvas *canvas;
  fld::Toolbar *bar;
  std::vector<GlyphButton *> tool_btns;
  Fl_Menu_Button *color_btn;
  Fl_Choice *size_choice;
  fld::FlatButton *undo_btn, *redo_btn;
  Fl_Box *status;

  explicit EditorWindow(Image img) : Fl_Double_Window(900, 640, "Snip"), base(std::move(img)) {
    const fld::Palette &p = fld::palette();
    int sw, sh, sx, sy;
    Fl::screen_work_area(sx, sy, sw, sh);
    int W = std::min(sw - 40, std::max(880, base.w + 40)), H = std::min(sh - 40, std::max(480, base.h + 120));
    size(W, H);
    const int TB = 44, SB = 24;
    bar = new fld::Toolbar(0, 0, W, TB);
    bar->color(p.bg2);
    bar->add_button("document-new", "New snip (Ctrl+N)", [](Fl_Widget *w, void *) { new_snip(S.mode, 0); }, nullptr,
                    "New");
    bar->add_separator();
    static const struct { Tool t; Glyph g; const char *tip; } tools[] = {
        {Tool::Pen, Glyph::Pen, "Pen (P)"},
        {Tool::Highlighter, Glyph::Highlighter, "Highlighter (H)"},
        {Tool::Line, Glyph::Line, "Line (L)"},
        {Tool::Arrow, Glyph::Arrow, "Arrow (A)"},
        {Tool::Box, Glyph::Box, "Rectangle (R)"},
        {Tool::Ellipse, Glyph::Ellipse, "Ellipse (E)"},
        {Tool::Text, Glyph::Text, "Text (T)"},
        {Tool::Pixelate, Glyph::Pixelate, "Pixelate, to hide something (X)"},
        {Tool::Crop, Glyph::Crop, "Crop (C)"},
        {Tool::Eraser, Glyph::Eraser, "Eraser: click a drawing to remove it (D)"}};
    for (auto &t : tools) {
      auto *b = new GlyphButton(bar->next_x(), 4, 36, 36, t.g, t.tip);
      b->color(p.bg2);
      b->callback([](Fl_Widget *w, void *d) { ((EditorWindow *)w->window())->set_tool((Tool)(long)d); },
                  (void *)(long)t.t);
      bar->add_widget(b);
      tool_btns.push_back(b);
    }
    bar->add_separator();
    color_btn = new Fl_Menu_Button(bar->next_x(), 8, 44, 28);
    color_btn->tooltip("Color");
    for (size_t i = 0; i < sizeof kColors / sizeof kColors[0]; i++)
      color_btn->add(kColorNames[i], 0, [](Fl_Widget *w, void *d) {
        EditorWindow *e = (EditorWindow *)w->window();
        e->color = (int)(long)d;
        e->color_btn->color(kColors[e->color]);
        e->color_btn->redraw();
      }, (void *)(long)i);
    color_btn->color(kColors[0]);
    color_btn->box(FL_ROUNDED_BOX);
    bar->add_widget(color_btn);
    size_choice = new Fl_Choice(bar->next_x() + 4, 8, 82, 28);
    size_choice->add("Thin");
    size_choice->add("Medium");
    size_choice->add("Thick");
    size_choice->value(1);
    size_choice->tooltip("Line width");
    size_choice->callback([](Fl_Widget *w, void *) {
      EditorWindow *e = (EditorWindow *)w->window();
      static const int sizes[] = {2, 4, 8};
      e->pen_size = sizes[((Fl_Choice *)w)->value()];
    });
    bar->add_widget(size_choice);
    bar->add_separator();
    undo_btn = bar->add_button("edit-undo", "Undo (Ctrl+Z)", [](Fl_Widget *w, void *) {
      ((EditorWindow *)w->window())->undo();
    }, nullptr);
    redo_btn = bar->add_button("edit-redo", "Redo (Ctrl+Shift+Z)", [](Fl_Widget *w, void *) {
      ((EditorWindow *)w->window())->redo();
    }, nullptr);
    bar->add_separator();
    bar->add_button("edit-copy", "Copy (Ctrl+C)", [](Fl_Widget *w, void *) { ((EditorWindow *)w->window())->copy(); },
                    nullptr);
    bar->add_button("document-save", "Save (Ctrl+S)", [](Fl_Widget *w, void *) { ((EditorWindow *)w->window())->save(); },
                    nullptr);
    bar->add_button("document-save-as", "Save As (Ctrl+Shift+S)", [](Fl_Widget *w, void *) {
      ((EditorWindow *)w->window())->save_as();
    }, nullptr);
    fld::FlatButton *more = bar->add_button("open-menu", "More", [](Fl_Widget *w, void *) {
      ((EditorWindow *)w->window())->more_menu(w);
    }, nullptr);
    if (!fld::icon("open-menu", 20)) more->label("…");
    bar->end();
    canvas = new Canvas(0, TB, W, H - TB - SB, this);
    status = new Fl_Box(0, H - SB, W, SB);
    status->box(FL_FLAT_BOX);
    status->color(p.bg2);
    status->labelsize(12);
    status->labelcolor(p.fg_dim);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    end();
    resizable(canvas);
    size_range(560, 360);
    fld::set_window_icon(this, "applets-screenshooter");
    callback([](Fl_Widget *w, void *) { ((EditorWindow *)w)->close(); });
    set_tool(Tool::Pen);
    rebuild();
    g_editors.push_back(this);
    fld::set_context_menu(canvas, [this](fld::Menu &m) { context_menu(m); }, false);
  }

  ~EditorWindow() override { g_editors.erase(std::remove(g_editors.begin(), g_editors.end(), this), g_editors.end()); }

  // the visible part: the last crop, or all
  Rect view_rect() const {
    Rect r{0, 0, base.w, base.h};
    for (auto &s : shapes)
      if (s.tool == Tool::Crop && s.pts.size() == 2)
        r = snip::clip(snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second), base.w, base.h);
    return r;
  }

  void set_tool(Tool t) {
    tool = t;
    for (size_t i = 0; i < tool_btns.size(); i++) tool_btns[i]->active_look((int)i == (int)t);
    canvas->window() ? canvas->window()->cursor(t == Tool::Text ? FL_CURSOR_INSERT : FL_CURSOR_CROSS) : (void)0;
  }

  // work = base + the highlighter and pixelate shapes; called after changes
  void rebuild() {
    work = base;
    for (auto &s : shapes) apply_pixels(work, s);
    work_img = rgb_image(work);
    update_status();
    canvas->redraw();
    undo_stack.empty() ? undo_btn->deactivate() : undo_btn->activate();
    redo_stack.empty() ? redo_btn->deactivate() : redo_btn->activate();
    std::string t = (path.empty() ? std::string("Snip") : fld::basename(path)) + (dirty ? " *" : "") + " — Snip";
    copy_label(t.c_str());
  }

  void update_status() {
    Rect r = view_rect();
    std::string s = std::to_string(r.w) + " × " + std::to_string(r.h) + " pixels";
    if (!path.empty()) s += "   " + path;
    status->copy_label(fld::escape_at(s).c_str());
  }

  static void apply_pixels(Image &img, const Shape &s) {
    if (s.tool == Tool::Pixelate && s.pts.size() == 2) {
      snip::pixelate(img, snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second), 10);
    } else if (s.tool == Tool::Highlighter && !s.pts.empty()) {
      // a marker: each channel darkened towards the color (multiply)
      uint8_t cr, cg, cb;
      Fl::get_color(s.color, cr, cg, cb);
      double rad = s.size * 2.5;
      std::vector<uint8_t> hit((size_t)img.w * img.h, 0);
      auto seg = [&](Point a, Point b) {
        int x1 = (int)std::floor(std::min(a.first, b.first) - rad), x2 = (int)std::ceil(std::max(a.first, b.first) + rad);
        int y1 = (int)std::floor(std::min(a.second, b.second) - rad), y2 = (int)std::ceil(std::max(a.second, b.second) + rad);
        double dx = b.first - a.first, dy = b.second - a.second, len2 = dx * dx + dy * dy;
        for (int y = std::max(0, y1); y <= std::min(img.h - 1, y2); y++)
          for (int x = std::max(0, x1); x <= std::min(img.w - 1, x2); x++) {
            double t = len2 > 0 ? std::max(0.0, std::min(1.0, ((x - a.first) * dx + (y - a.second) * dy) / len2)) : 0;
            double px = a.first + t * dx - x, py = a.second + t * dy - y;
            if (px * px + py * py <= rad * rad) hit[(size_t)y * img.w + x] = 1;
          }
      };
      if (s.pts.size() == 1) seg(s.pts[0], s.pts[0]);
      for (size_t i = 1; i < s.pts.size(); i++) seg(s.pts[i - 1], s.pts[i]);
      for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++)
          if (hit[(size_t)y * img.w + x]) {
            uint8_t *p = img.at(x, y);
            p[0] = (uint8_t)(p[0] * cr / 255);
            p[1] = (uint8_t)(p[1] * cg / 255);
            p[2] = (uint8_t)(p[2] * cb / 255);
          }
    }
  }

  // the vector shapes, drawn with FLTK at offset ox, oy and zoom z
  static void draw_shape(const Shape &s, double ox, double oy, double z) {
    auto X = [&](double v) { return ox + v * z; };
    auto Y = [&](double v) { return oy + v * z; };
    fl_color(s.color);
    int lw = std::max(1, (int)lround(s.size * z));
    switch (s.tool) {
      case Tool::Pen:
        fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, lw);
        if (s.pts.size() == 1) {
          fl_pie((int)(X(s.pts[0].first) - lw / 2.0), (int)(Y(s.pts[0].second) - lw / 2.0), lw, lw, 0, 360);
        } else {
          fl_begin_line();
          for (auto &p : s.pts) fl_vertex(X(p.first), Y(p.second));
          fl_end_line();
        }
        break;
      case Tool::Line:
      case Tool::Arrow:
        if (s.pts.size() < 2) break;
        fl_line_style(FL_SOLID | FL_CAP_ROUND, lw);
        fl_begin_line();
        fl_vertex(X(s.pts[0].first), Y(s.pts[0].second));
        fl_vertex(X(s.pts[1].first), Y(s.pts[1].second));
        fl_end_line();
        if (s.tool == Tool::Arrow) {
          fl_line_style(0);
          draw_arrow_head(X(s.pts[0].first), Y(s.pts[0].second), X(s.pts[1].first), Y(s.pts[1].second),
                          std::max(10.0, s.size * 4.0 * z));
        }
        break;
      case Tool::Box:
      case Tool::Ellipse: {
        if (s.pts.size() < 2) break;
        Rect r = snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second);
        fl_line_style(FL_SOLID, lw);
        int x = (int)X(r.x), y = (int)Y(r.y), w = (int)(r.w * z), h = (int)(r.h * z);
        if (s.tool == Tool::Box) fl_rect(x, y, w, h);
        else fl_arc(x, y, w, h, 0, 360);
        break;
      }
      case Tool::Text:
        if (s.pts.empty()) break;
        fl_font(FL_HELVETICA_BOLD, std::max(6, (int)lround(s.size * 5 * z)));
        fl_draw(s.text.c_str(), (int)X(s.pts[0].first), (int)Y(s.pts[0].second));
        break;
      default:
        break;
    }
    fl_line_style(0);
  }

  // the finished picture: work, the vector shapes on it, cropped
  Image flatten() {
    Image out = work;
    bool vec = std::any_of(shapes.begin(), shapes.end(), [](const Shape &s) {
      return s.tool == Tool::Pen || s.tool == Tool::Line || s.tool == Tool::Arrow || s.tool == Tool::Box ||
             s.tool == Tool::Ellipse || s.tool == Tool::Text;
    });
    if (vec) {
      Fl_Image_Surface surf(work.w, work.h);
      Fl_Surface_Device::push_current(&surf);
      fl_color(FL_WHITE);
      fl_rectf(0, 0, work.w, work.h);
      work_img->draw(0, 0);
      for (auto &s : shapes) draw_shape(s, 0, 0, 1);
      std::unique_ptr<Fl_RGB_Image> shot(surf.image());
      Fl_Surface_Device::pop_current();
      if (shot && shot->d() >= 3 && shot->data_w() == work.w && shot->data_h() == work.h) {
        const uint8_t *d = (const uint8_t *)shot->data()[0];
        int ld = shot->ld() ? shot->ld() : shot->data_w() * shot->d();
        for (int y = 0; y < work.h; y++)
          for (int x = 0; x < work.w; x++) {
            const uint8_t *s = d + (size_t)y * ld + (size_t)x * shot->d();
            uint8_t *o = out.at(x, y);
            o[0] = s[0];
            o[1] = s[1];
            o[2] = s[2];
          }
      }
    }
    Rect v = view_rect();
    if (v.x || v.y || v.w != out.w || v.h != out.h) out = snip::crop(out, v);
    return out;
  }

  void push_undo() {
    undo_stack.push_back(shapes);
    if (undo_stack.size() > 100) undo_stack.erase(undo_stack.begin());
    redo_stack.clear();
    dirty = true;
  }
  void undo() {
    if (undo_stack.empty()) return;
    redo_stack.push_back(shapes);
    shapes = undo_stack.back();
    undo_stack.pop_back();
    rebuild();
  }
  void redo() {
    if (redo_stack.empty()) return;
    undo_stack.push_back(shapes);
    shapes = redo_stack.back();
    redo_stack.pop_back();
    rebuild();
  }

  void add_shape(const Shape &s) {
    push_undo();
    shapes.push_back(s);
    if (s.tool == Tool::Crop) canvas->fit = true;
    rebuild();
  }

  // the topmost shape near an image point (for the eraser)
  int shape_at(Point p) const {
    for (int i = (int)shapes.size() - 1; i >= 0; i--) {
      const Shape &s = shapes[(size_t)i];
      if (s.tool == Tool::Crop) continue;
      Rect b = snip::bounds(s.pts);
      int pad = s.size * 3 + 4;
      if (s.tool == Tool::Text) {
        b.y -= s.size * 5;
        b.w = std::max(b.w, (int)s.text.size() * s.size * 3);
        b.h = s.size * 6;
      }
      if (Rect{b.x - pad, b.y - pad, b.w + 2 * pad, b.h + 2 * pad}.contains(p.first, p.second)) return i;
    }
    return -1;
  }

  void copy() {
    copy_image(flatten(), false);
    status->copy_label("Copied to the clipboard");
  }

  bool save_to(const std::string &p) {
    if (!write_png(flatten(), p)) {
      fl_alert("Could not save %s.", fld::escape_at(p).c_str());
      return false;
    }
    path = p;
    dirty = false;
    rebuild();
    return true;
  }
  bool save() {
    if (path.empty()) return save_as();
    return save_to(path);
  }
  bool save_as() {
    Fl_Native_File_Chooser fc;
    fc.title("Save Snip");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    fc.filter("PNG image\t*.png");
    std::string dir = path.empty() ? screenshots_dir() : fld::dirname(path);
    fld::mkdir_p(dir);
    fc.directory(dir.c_str());
    fc.preset_file(path.empty() ? snip::file_name(time(nullptr)).c_str() : fld::basename(path).c_str());
    if (fc.show() != 0 || !fc.filename()) return false;
    std::string p = fc.filename();
    if (fld::lower(fld::extension(p)) != "png") p += ".png";
    return save_to(p);
  }

  // a file of the picture as it is now (saved, or a temporary one)
  std::string current_file() {
    if (!path.empty() && !dirty) return path;
    std::string tmp = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-snip-" + snip::file_name(time(nullptr));
    return write_png(flatten(), tmp) ? tmp : "";
  }

  void more_menu(Fl_Widget *w) {
    fld::Menu m;
    m.item("Open in Image Viewer", [this] {
      std::string f = current_file();
      if (!f.empty()) fld::spawn({"fl-view", f});
    });
    m.item("Open With Another Program…", [this] {
      std::string f = current_file();
      if (!f.empty()) fld::spawn({"xdg-open", f});
    });
    m.item("Show in Files", [this] { fld::spawn({"fl-files", fld::dirname(path)}); },
           path.empty() ? FL_MENU_INACTIVE : 0);
    m.item("Print…", [this] {
      std::string f = current_file();
      if (f.empty()) return;
      if (fl_choice("Print the snip on the default printer?", "Cancel", "Print", nullptr) == 1 &&
          !fld::spawn({"lp", f}))
        fl_alert("Printing needs CUPS (lp).");
    }, fld::which("lp").empty() ? FL_MENU_INACTIVE : 0).divider();
    m.submenu("New Snip");
    for (Mode md : {Mode::Rect, Mode::Freeform, Mode::Window, Mode::Full})
      m.item(mode_name(md), [md] { new_snip(md, 0); });
    m.end_submenu();
    m.submenu("New Snip in 5 Seconds");
    for (Mode md : {Mode::Rect, Mode::Freeform, Mode::Window, Mode::Full})
      m.item(mode_name(md), [md] { new_snip(md, 5); });
    m.end_submenu();
    m.item("Settings…", [] { show_settings(); });
    m.item("About", [] {
      fld::about_dialog("Snip", "applets-screenshooter", "Screenshots of a part of the screen, a window or all of it.");
    });
    m.popup(w->x(), w->y() + w->h());
  }

  void context_menu(fld::Menu &m) {
    m.item("Copy", [this] { copy(); }, 0, FL_CTRL + 'c');
    m.item("Save", [this] { save(); }, 0, FL_CTRL + 's');
    m.item("Save As…", [this] { save_as(); }, 0, FL_CTRL + FL_SHIFT + 's').divider();
    m.item("Undo", [this] { undo(); }, undo_stack.empty() ? FL_MENU_INACTIVE : 0, FL_CTRL + 'z');
    m.item("Redo", [this] { redo(); }, redo_stack.empty() ? FL_MENU_INACTIVE : 0, FL_CTRL + FL_SHIFT + 'z');
    m.item("Clear Drawings", [this] {
      push_undo();
      shapes.clear();
      rebuild();
    }, shapes.empty() ? FL_MENU_INACTIVE : 0).divider();
    static const char *names[] = {"Pen", "Highlighter", "Line", "Arrow", "Rectangle", "Ellipse", "Text", "Pixelate",
                                  "Crop", "Eraser"};
    m.submenu("Tool");
    for (int i = 0; i < 10; i++) m.radio(names[i], (int)tool == i, [this, i] { set_tool((Tool)i); });
    m.end_submenu();
    m.submenu("Color");
    for (int i = 0; i < (int)(sizeof kColors / sizeof kColors[0]); i++)
      m.radio(kColorNames[i], color == i, [this, i] {
        color = i;
        color_btn->color(kColors[i]);
        color_btn->redraw();
      });
    m.end_submenu();
    m.divider();
    m.toggle("Fit to Window", canvas->fit, [this] {
      canvas->fit = !canvas->fit;
      if (!canvas->fit) canvas->zoom = 1;
      canvas->redraw();
    });
    m.item("Actual Size", [this] {
      canvas->fit = false;
      canvas->zoom = 1;
      canvas->redraw();
    }, 0, FL_CTRL + '0');
  }

  int handle(int e) override {
    if (e == FL_KEYBOARD || e == FL_SHORTCUT) {
      int k = Fl::event_key(), st = Fl::event_state();
      bool ctrl = st & FL_CTRL, shift = st & FL_SHIFT;
      if (ctrl) {
        switch (k) {
          case 'z': shift ? redo() : undo(); return 1;
          case 'y': redo(); return 1;
          case 'c': copy(); return 1;
          case 's': shift ? save_as() : save(); return 1;
          case 'n': new_snip(S.mode, 0); return 1;
          case 'w': close(); return 1;
          case '0': canvas->fit = false; canvas->zoom = 1; canvas->redraw(); return 1;
          case '=': case '+': canvas->fit = false; canvas->zoom = std::min(8.0, canvas->zoom * 1.25); canvas->redraw(); return 1;
          case '-': canvas->fit = false; canvas->zoom = std::max(0.1, canvas->zoom / 1.25); canvas->redraw(); return 1;
        }
      } else if (e == FL_KEYBOARD && !(st & FL_ALT)) {
        static const char keys[] = "phlaretxcd";
        const char *p = k < 128 ? strchr(keys, k) : nullptr;
        if (p && *p) {
          set_tool((Tool)(p - keys));
          return 1;
        }
        if (k == FL_Escape) return 1; // Esc does not close the editor
      }
    }
    return Fl_Double_Window::handle(e);
  }

  void close() {
    if (dirty && path.empty()) {
      int c = fl_choice("Save the snip before closing?", "Cancel", "Save", "Don't Save");
      if (c == 0) return;
      if (c == 1 && !save()) return;
    } else if (dirty) {
      int c = fl_choice("Save the changes to %s?", "Cancel", "Save", "Don't Save", fld::basename(path).c_str());
      if (c == 0) return;
      if (c == 1 && !save()) return;
    }
    hide();
    Fl::delete_widget(this);
  }
};

double Canvas::ox() const {
  Rect v = ed->view_rect();
  double W = v.w * zoom;
  return x() + std::max(0.0, (w() - W) / 2) - v.x * zoom;
}
double Canvas::oy() const {
  Rect v = ed->view_rect();
  double H = v.h * zoom;
  return y() + std::max(0.0, (h() - H) / 2) - v.y * zoom;
}

Point Canvas::to_image(int ex, int ey) const {
  return {(int)lround((ex - ox()) / zoom), (int)lround((ey - oy()) / zoom)};
}

void Canvas::draw() {
  Rect v = ed->view_rect();
  if (fit) zoom = std::min(1.0, std::min((w() - 16.0) / std::max(1, v.w), (h() - 16.0) / std::max(1, v.h)));
  fl_push_clip(x(), y(), w(), h());
  fl_color(fl_color_average(fld::palette().bg, FL_BLACK, 0.85f));
  fl_rectf(x(), y(), w(), h());
  double X0 = ox(), Y0 = oy();
  int vx = (int)lround(X0 + v.x * zoom), vy = (int)lround(Y0 + v.y * zoom);
  int vw = (int)lround(v.w * zoom), vh = (int)lround(v.h * zoom);
  // a checkerboard under transparent parts
  fl_push_clip(vx, vy, vw, vh);
  for (int yy = vy; yy < vy + vh; yy += 8)
    for (int xx = vx; xx < vx + vw; xx += 8) {
      fl_color(((xx - vx) / 8 + (yy - vy) / 8) % 2 ? fl_rgb_color(0xcc, 0xcc, 0xcc) : FL_WHITE);
      fl_rectf(xx, yy, 8, 8);
    }
  Fl_RGB_Image *img = ed->work_img.get();
  img->scale((int)lround(ed->work.w * zoom), (int)lround(ed->work.h * zoom), 0, 1);
  img->draw((int)lround(X0), (int)lround(Y0));
  for (auto &s : ed->shapes) EditorWindow::draw_shape(s, X0, Y0, zoom);
  if (ed->live) {
    const Shape &s = *ed->live;
    if (s.tool == Tool::Pixelate || s.tool == Tool::Crop || s.tool == Tool::Highlighter) {
      // a preview: the rectangle, or the stroke
      fl_color(s.tool == Tool::Highlighter ? s.color : fld::palette().accent);
      if (s.tool == Tool::Highlighter) {
        fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, std::max(1, (int)lround(s.size * 5 * zoom)));
        fl_begin_line();
        for (auto &p : s.pts) fl_vertex(X0 + p.first * zoom, Y0 + p.second * zoom);
        fl_end_line();
      } else if (s.pts.size() == 2) {
        Rect r = snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second);
        fl_line_style(FL_DASH, 2);
        fl_rect((int)(X0 + r.x * zoom), (int)(Y0 + r.y * zoom), (int)(r.w * zoom), (int)(r.h * zoom));
      }
      fl_line_style(0);
    } else {
      EditorWindow::draw_shape(s, X0, Y0, zoom);
    }
  }
  fl_pop_clip();
  fl_pop_clip();
}

int Canvas::handle(int e) {
  switch (e) {
    case FL_ENTER:
      window()->cursor(ed->tool == Tool::Text ? FL_CURSOR_INSERT : ed->tool == Tool::Eraser ? FL_CURSOR_HAND : FL_CURSOR_CROSS);
      return 1;
    case FL_LEAVE:
      window()->cursor(FL_CURSOR_DEFAULT);
      return 1;
    case FL_MOUSEWHEEL:
      if (Fl::event_state() & FL_CTRL) {
        fit = false;
        zoom = std::max(0.1, std::min(8.0, zoom * (Fl::event_dy() < 0 ? 1.25 : 0.8)));
        redraw();
        return 1;
      }
      return 0;
    case FL_PUSH: {
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      Point p = to_image(Fl::event_x(), Fl::event_y());
      if (ed->tool == Tool::Eraser) {
        int i = ed->shape_at(p);
        if (i >= 0) {
          ed->push_undo();
          ed->shapes.erase(ed->shapes.begin() + i);
          ed->rebuild();
        }
        return 1;
      }
      if (ed->tool == Tool::Text) {
        const char *t = fl_input("Text:", "");
        if (t && *t) {
          Shape s{Tool::Text, kColors[ed->color], ed->pen_size, {p}, t};
          // the click is the top left of the text: fl_draw takes the baseline
          s.pts[0].second += s.size * 5;
          ed->add_shape(s);
        }
        return 1;
      }
      ed->live_shape = Shape{ed->tool, kColors[ed->color], ed->pen_size, {p}, ""};
      if (ed->tool != Tool::Pen && ed->tool != Tool::Highlighter) ed->live_shape.pts.push_back(p);
      ed->live = &ed->live_shape;
      redraw();
      return 1;
    }
    case FL_DRAG: {
      if (!ed->live) return 1;
      Point p = to_image(Fl::event_x(), Fl::event_y());
      Shape &s = ed->live_shape;
      if (s.tool == Tool::Pen || s.tool == Tool::Highlighter) {
        if (s.pts.back() != p) s.pts.push_back(p);
      } else {
        if (Fl::event_state() & FL_SHIFT && (s.tool == Tool::Line || s.tool == Tool::Arrow)) {
          // Shift: horizontal, vertical or 45 degrees
          int dx = p.first - s.pts[0].first, dy = p.second - s.pts[0].second;
          if (std::abs(dx) > 2 * std::abs(dy)) p.second = s.pts[0].second;
          else if (std::abs(dy) > 2 * std::abs(dx)) p.first = s.pts[0].first;
          else p.second = s.pts[0].second + (dy < 0 ? -1 : 1) * std::abs(dx);
        }
        s.pts[1] = p;
      }
      redraw();
      return 1;
    }
    case FL_RELEASE: {
      if (!ed->live) return 1;
      Shape s = ed->live_shape;
      ed->live = nullptr;
      bool rect_tool = s.tool == Tool::Box || s.tool == Tool::Ellipse || s.tool == Tool::Pixelate || s.tool == Tool::Crop;
      if (rect_tool && s.pts.size() == 2) {
        Rect r = snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second);
        if (r.w < 3 || r.h < 3) {
          redraw();
          return 1;
        }
      }
      if ((s.tool == Tool::Line || s.tool == Tool::Arrow) && s.pts.size() == 2 && s.pts[0] == s.pts[1]) {
        redraw();
        return 1;
      }
      ed->add_shape(s);
      return 1;
    }
  }
  return Fl_Widget::handle(e);
}

static EditorWindow *open_editor(Image img, const std::string &saved_path) {
  auto *e = new EditorWindow(std::move(img));
  e->path = saved_path;
  e->rebuild();
  e->show();
  return e;
}

// ---------------------------------------------------------------------------
// The main window: New, mode, delay

static Fl_Double_Window *g_main;
static Fl_Choice *g_mode_choice, *g_delay_choice;
static const int kDelays[] = {0, 3, 5, 10};

// a snip from the main window or an editor: it opens in the editor
static void new_snip(Mode mode, int delay) {
  std::string err;
  Image img = take_snip(mode, delay, err);
  if (!err.empty()) {
    fl_alert("%s", err.c_str());
    return;
  }
  if (img.empty()) return;
  std::string saved;
  if (S.autosave) saved = auto_save(img);
  if (S.copy) copy_image(img, false);
  if (g_main) g_main->hide();
  open_editor(std::move(img), saved);
}

static void show_settings() {
  Fl_Double_Window dlg(420, 230, "Snip Settings");
  Fl_Check_Button copy(16, 14, 390, 26, "Copy each snip to the clipboard");
  copy.value(S.copy);
  Fl_Check_Button save(16, 42, 390, 26, "Save each snip to Pictures/Screenshots");
  save.value(S.autosave);
  Fl_Check_Button cursor(16, 70, 390, 26, "Include the mouse pointer");
  cursor.value(S.cursor);
  Fl_Check_Button note(16, 98, 390, 26, "Show a notification after a quick snip (Print key)");
  note.value(S.notify);
  Fl_Button cancel(208, 186, 96, 30, "Cancel");
  Fl_Return_Button ok(310, 186, 96, 30, "Save");
  int result = -1;
  cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 0; }, &result);
  ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
  dlg.end();
  dlg.set_modal();
  dlg.show();
  while (result < 0 && dlg.shown()) Fl::wait();
  if (result != 1) return;
  S.copy = copy.value();
  S.autosave = save.value();
  S.cursor = cursor.value();
  S.notify = note.value();
  S.save();
}

static void make_main_window() {
  const fld::Palette &p = fld::palette();
#ifdef FLD_HAVE_RECORD
  const int RX = 100; // room for the Record button
#else
  const int RX = 0;
#endif
  g_main = new Fl_Double_Window(520 + RX, 64, "Snip");
  g_main->color(p.bg);
  fld::FlatButton *nw = new fld::FlatButton(8, 12, 96, 40, "New");
  nw->icon_name("applets-screenshooter", 22);
  nw->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER | FL_ALIGN_IMAGE_NEXT_TO_TEXT);
  nw->tooltip("Take a snip (Ctrl+N)");
  nw->shortcut(FL_CTRL + 'n');
  nw->callback([](Fl_Widget *, void *) {
    S.mode = (Mode)g_mode_choice->value();
    S.delay = kDelays[g_delay_choice->value()];
    S.save();
    new_snip(S.mode, S.delay);
    if (g_editors.empty() && g_main) g_main->show();
  });
#ifdef FLD_HAVE_RECORD
  fld::FlatButton *rb = new fld::FlatButton(108, 12, 96, 40, "Record");
  rb->icon_name("media-record", 22);
  rb->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER | FL_ALIGN_IMAGE_NEXT_TO_TEXT);
  rb->tooltip("Record a screen or a window (Ctrl+R)");
  rb->shortcut(FL_CTRL + 'r');
  rb->callback([](Fl_Widget *, void *) {
    // a separate process: the recording outlives this window
    fld::spawn({"fl-snip", "--record"});
    if (g_editors.empty() && g_main) g_main->hide();
  });
#endif
  g_mode_choice = new Fl_Choice(170 + RX, 18, 130, 28, "Mode:");
  for (Mode m : {Mode::Rect, Mode::Freeform, Mode::Window, Mode::Full}) g_mode_choice->add(mode_name(m));
  g_mode_choice->value((int)S.mode);
  g_delay_choice = new Fl_Choice(356 + RX, 18, 100, 28, "Delay:");
  g_delay_choice->add("None");
  g_delay_choice->add("3 seconds");
  g_delay_choice->add("5 seconds");
  g_delay_choice->add("10 seconds");
  int di = 0;
  for (int i = 0; i < 4; i++)
    if (kDelays[i] == S.delay) di = i;
  g_delay_choice->value(di);
  fld::FlatButton *more = new fld::FlatButton(470 + RX, 12, 40, 40);
  more->icon_name("open-menu", 20);
  if (!fld::icon("open-menu", 20)) more->label("…");
  more->tooltip("More");
  more->callback([](Fl_Widget *w, void *) {
    fld::Menu m;
    m.item("Settings…", [] { show_settings(); });
#ifdef FLD_HAVE_RECORD
    m.item("Record with Sound…", [] {
      fld::spawn({"fl-snip", "--record", "--audio"});
      if (g_editors.empty() && g_main) g_main->hide();
    });
#endif
    m.item("Open Screenshots Folder", [] {
      fld::mkdir_p(screenshots_dir());
      fld::spawn({"fl-files", screenshots_dir()});
    });
    m.item("Open Image…", [] {
      Fl_Native_File_Chooser fc;
      fc.title("Open Image");
      fc.filter("Images\t*.{png,jpg,jpeg,bmp,gif}");
      fc.directory(screenshots_dir().c_str());
      if (fc.show() != 0 || !fc.filename()) return;
      std::unique_ptr<Fl_Image> im(fld::load_image(fc.filename()));
      Fl_RGB_Image *rgb = dynamic_cast<Fl_RGB_Image *>(im.get());
      if (!rgb || rgb->d() < 3) {
        fl_alert("Could not open the image.");
        return;
      }
      Image img;
      img.w = rgb->data_w();
      img.h = rgb->data_h();
      img.px.resize((size_t)img.w * img.h * 4);
      const uint8_t *d = (const uint8_t *)rgb->data()[0];
      int ld = rgb->ld() ? rgb->ld() : img.w * rgb->d();
      for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
          const uint8_t *s = d + (size_t)y * ld + (size_t)x * rgb->d();
          uint8_t *o = img.at(x, y);
          o[0] = s[0];
          o[1] = s[1];
          o[2] = s[2];
          o[3] = rgb->d() == 4 ? s[3] : 255;
        }
      g_main->hide();
      open_editor(std::move(img), fc.filename());
    }).divider();
    m.item("About", [] {
      fld::about_dialog("Snip", "applets-screenshooter", "Screenshots of a part of the screen, a window or all of it.");
    });
    m.popup(w->x(), w->y() + w->h());
  });
  g_main->end();
  fld::set_window_icon(g_main, "applets-screenshooter");
}

// ---------------------------------------------------------------------------
// Quick snips (the Print keys): save, copy, notify, print the path

static int quick(Mode mode, int delay, bool edit) {
  std::string err;
  Image img = take_snip(mode, delay, err);
  if (!err.empty()) {
    fprintf(stderr, "fl-snip: %s\n", err.c_str());
    fld::Notification n;
    n.summary = "Screenshot";
    n.body = err;
    n.icon = "dialog-error";
    n.transient = true;
    fld::notify(n);
    return 1;
  }
  if (img.empty()) return 1; // cancelled
  std::string path = S.autosave || !edit ? auto_save(img) : "";
  if (S.copy) copy_image(img, !edit);
  if (!path.empty()) printf("%s\n", path.c_str());
  fflush(stdout);
  if (edit) {
    open_editor(std::move(img), path);
    return Fl::run();
  }
  if (!S.notify || path.empty()) return 0;
  // the notification's buttons work while fl-snip waits a little for them
  fld::Notification n;
  n.app_name = "Snip";
  n.desktop_entry = "fl-snip";
  n.summary = "Screenshot taken";
  n.body = fld::basename(path) + (S.copy ? " — copied to the clipboard" : "");
  n.icon = path;
  n.transient = true;
  n.actions = {{"default", "Open"}, {"edit", "Edit"}, {"folder", "Show in Files"}};
  static bool answered = false;
  static std::string action;
  fld::notify(n, [](const std::string &a) {
    action = a;
    answered = true;
  });
  double until = now_s() + 15;
  while (!answered && now_s() < until) Fl::wait(0.2);
  if (action == "default") fld::spawn({"fl-view", path});
  else if (action == "folder") fld::spawn({"fl-files", fld::dirname(path)});
  else if (action == "edit") {
    open_editor(std::move(img), path);
    return Fl::run();
  }
  return 0;
}

static const char *kUsage =
    "Usage: fl-snip [OPTION]\n"
    "  (none)          the Snip window: choose a mode and a delay, then New\n"
    "  --snip          choose a part of the screen, then edit it\n"
    "  --area          choose a rectangle; save and copy it (Shift+Print)\n"
    "  --window        choose a window; save and copy it\n"
    "  --full          the whole screen; save and copy it (Print)\n"
    "  --freeform      draw a shape around a part; save and copy it\n"
    "  --delay N       wait N seconds first\n"
    "  --edit FILE     open an image in the editor\n"
#ifdef FLD_HAVE_RECORD
    "  --record        record a screen or a window (WebM) until Stop\n"
    "    --audio         with the desktop's sound\n"
    "    --seconds N     stop after N seconds\n"
    "    -o FILE         the file (default: Videos/Screencasts)\n"
    "    --record-node N record PipeWire node N (no portal)\n"
    "    --no-window     no time window and no notification\n"
#endif
    "Quick snips print the saved file's path.\n";

int main(int argc, char **argv) {
  enum { MAIN, SNIP, QUICK, EDIT, RECORD } what = MAIN;
#ifdef FLD_HAVE_RECORD
  record::Options rec;
#endif
  Mode mode = Mode::Rect;
  int delay = -1;
  std::string edit_file;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      fputs(kUsage, stdout);
      return 0;
    }
    if (a == "-v" || a == "--version") {
      printf("fl-snip %s\n", FLD_VERSION);
      return 0;
    }
    if (a == "--snip") what = SNIP;
    else if (a == "--area") { what = QUICK; mode = Mode::Rect; }
    else if (a == "--window") { what = QUICK; mode = Mode::Window; }
    else if (a == "--full") { what = QUICK; mode = Mode::Full; }
    else if (a == "--freeform") { what = QUICK; mode = Mode::Freeform; }
    else if (a == "--delay" && i + 1 < argc) delay = std::max(0, std::min(60, atoi(argv[++i])));
    else if (a == "--edit" && i + 1 < argc) { what = EDIT; edit_file = argv[++i]; }
#ifdef FLD_HAVE_RECORD
    else if (a == "--record") what = RECORD;
    else if (a == "--audio") rec.audio = true;
    else if (a == "--seconds" && i + 1 < argc) rec.seconds = std::max(0, atoi(argv[++i]));
    else if ((a == "-o" || a == "--output") && i + 1 < argc) rec.output = argv[++i];
    else if (a == "--record-node" && i + 1 < argc) { what = RECORD; rec.node = (uint32_t)atol(argv[++i]); }
    else if (a == "--no-window") { rec.window = false; rec.notify = false; }
#endif
    else {
      fprintf(stderr, "fl-snip: unknown option %s\n%s", a.c_str(), kUsage);
      return 2;
    }
  }
#ifdef FLD_HAVE_RECORD
  if (what == RECORD && !rec.window) return record::run(rec);
#endif
  fld::init_app("fl-snip", argc, argv);
  Fl::lock(); // notification answers come through Fl::awake
  fl_register_images();
  S.load();
  if (delay < 0) delay = what == MAIN ? S.delay : 0;
  switch (what) {
    case QUICK: return quick(mode, delay, false);
    case SNIP: return quick(S.mode, delay, true);
    case EDIT: {
      std::unique_ptr<Fl_Image> im(fld::load_image(edit_file));
      Fl_RGB_Image *rgb = dynamic_cast<Fl_RGB_Image *>(im.get());
      if (!rgb || rgb->d() < 3) {
        fprintf(stderr, "fl-snip: cannot open %s\n", edit_file.c_str());
        return 1;
      }
      Image img;
      img.w = rgb->data_w();
      img.h = rgb->data_h();
      img.px.resize((size_t)img.w * img.h * 4);
      const uint8_t *d = (const uint8_t *)rgb->data()[0];
      int ld = rgb->ld() ? rgb->ld() : img.w * rgb->d();
      for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++) {
          const uint8_t *s = d + (size_t)y * ld + (size_t)x * rgb->d();
          uint8_t *o = img.at(x, y);
          o[0] = s[0];
          o[1] = s[1];
          o[2] = s[2];
          o[3] = rgb->d() == 4 ? s[3] : 255;
        }
      open_editor(std::move(img), fld::abs_path(edit_file));
      return Fl::run();
    }
#ifdef FLD_HAVE_RECORD
    case RECORD: return record::run(rec);
#endif
    case MAIN:
      break;
  }
  make_main_window();
  g_main->show();
  return Fl::run();
}
