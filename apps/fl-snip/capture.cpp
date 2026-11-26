// fl-snip: capturing the screen and the selection overlay
#include "fld/i18n.h"
#include "snip_app.h"

// ---------------------------------------------------------------------------
// Capturing


bool grab_screen(Capture &cap, std::string &err) {
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
std::vector<snip::Window> wm_windows() {
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
std::unique_ptr<Fl_RGB_Image> rgb_image(const Image &img) {
  return std::unique_ptr<Fl_RGB_Image>(new Fl_RGB_Image(img.px.data(), img.w, img.h, 4));
}

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
    if (!tool()) make_bar();
    end();
    cursor(FL_CURSOR_CROSS);
  }

  // the chosen part in image pixels (empty: cancelled)
  Image result;
  bool done = false;
  std::string picked; // Pick: "#rrggbb"; Ruler: the measurement copied
  bool tool() const { return mode_ == Mode::Pick || mode_ == Mode::Ruler; }

  int handle(int e) override {
    if (Fl_Double_Window::handle(e) && (e == FL_PUSH || e == FL_RELEASE) && bar_ && Fl::event_inside(bar_)) return 1;
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
        if (mode_ == Mode::Pick) {
          pick(ex, ey);
          return 1;
        }
        dragging_ = true;
        measured_ = false;
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
        if (mode_ == Mode::Ruler) {
          // stays on the screen: Enter or C copies it, Esc closes
          measured_ = true;
          redraw();
          return 1;
        }
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
    if (mode_ == Mode::Ruler && (dragging_ || measured_)) {
      fl_color(p.accent);
      fl_line_style(FL_SOLID, 2);
      fl_line(ax_, ay_, mx_, my_);
      fl_line_style(0);
      fl_color(fl_color_average(p.accent, FL_WHITE, 0.5f));
      fl_line_style(FL_DOT, 1);
      fl_xyline(ax_, ay_, mx_);
      fl_yxline(mx_, ay_, my_);
      fl_line_style(0);
      std::string l = measurement();
      fl_font(FL_HELVETICA_BOLD, 13);
      int tw = (int)fl_width(l.c_str()) + 16, th = 24, tx = mx_ + 14, ty = my_ + 14;
      if (tx + tw > w()) tx = mx_ - tw - 14;
      if (ty + th > h()) ty = my_ - th - 14;
      fl_color(p.accent);
      fl_rounded_rectf(tx, ty, tw, th, 4);
      fl_color(p.accent_fg);
      fl_draw(l.c_str(), tx, ty, tw, th, FL_ALIGN_CENTER);
    }
    if (tool() && !dragging_ && !measured_) {
      fl_color(fl_color_average(p.accent, FL_WHITE, 0.6f));
      fl_line_style(FL_DOT, 1);
      fl_xyline(0, my_, w());
      fl_yxline(mx_, 0, h());
      fl_line_style(0);
      const char *hint = mode_ == Mode::Pick ? _("Click a pixel to copy its colour · Esc cancels")
                                             : _("Drag to measure · Enter copies · Esc closes");
      fl_font(FL_HELVETICA_BOLD, 14);
      int tw = (int)fl_width(hint) + 24;
      fl_color(p.bg);
      fl_rounded_rectf((w() - tw) / 2, 16, tw, 32, 6);
      fl_color(p.fg);
      fl_draw(hint, (w() - tw) / 2, 16, tw, 32, FL_ALIGN_CENTER);
    }
    if (mode_ == Mode::Rect || mode_ == Mode::Freeform || tool()) magnifier();
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
  bool measured_ = false;

  // image pixels: the ruler measures what the picture has, not logical units
  std::string measurement() const {
    return snip::ruler_label((int)lround(ax_ * cap_.sx), (int)lround(ay_ * cap_.sy), (int)lround(mx_ * cap_.sx),
                             (int)lround(my_ * cap_.sy));
  }

  bool pixel(int ex, int ey, uint8_t rgb[3]) const {
    int ix = (int)(ex * cap_.sx), iy = (int)(ey * cap_.sy);
    if (ix < 0 || iy < 0 || ix >= cap_.img.w || iy >= cap_.img.h) return false;
    const uint8_t *px = const_cast<Image &>(cap_.img).at(ix, iy);
    rgb[0] = px[0];
    rgb[1] = px[1];
    rgb[2] = px[2];
    return true;
  }

  void pick(int ex, int ey) {
    uint8_t c[3];
    if (!pixel(ex, ey, c)) return;
    picked = snip::hex_color(c[0], c[1], c[2]);
    done = true;
    hide();
  }

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
    if (tool()) {
      if (k == FL_Escape) finish(false);
      else if ((k == FL_Enter || k == FL_KP_Enter || k == 'c') && mode_ == Mode::Ruler && measured_) {
        picked = measurement();
        done = true;
        hide();
      } else if ((k == FL_Enter || k == FL_KP_Enter) && mode_ == Mode::Pick) {
        pick(mx_, my_);
      }
      return 1;
    }
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
    uint8_t c[3];
    if (mode_ == Mode::Pick && pixel(mx_, my_, c)) {
      snprintf(buf, sizeof buf, "%s", snip::hex_color(c[0], c[1], c[2]).c_str());
      fl_color(fl_rgb_color(c[0], c[1], c[2]));
      fl_rectf(bx + 4, by + S2 + 4, 16, 16);
    }
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
Image take_snip(Mode mode, int delay, std::string &err) {
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

// The colour picker and the ruler: a frozen picture of the screen; the colour
// ("#rrggbb") or the measurement is printed and copied. Returns 1 when cancelled.
int run_tool(Mode mode) {
  std::string err;
  double until = now_s() + 0.2;
  while (now_s() < until) Fl::wait(0.05);
  Capture cap;
  if (!grab_screen(cap, err)) {
    fprintf(stderr, "fl-snip: %s\n", err.c_str());
    return 1;
  }
  Overlay o(cap, mode);
  o.show();
  o.take_focus();
  while (!o.done && o.shown()) Fl::wait();
  if (o.picked.empty()) return 1;
  printf("%s\n", o.picked.c_str());
  fflush(stdout);
  // wl-copy keeps it on the clipboard after we exit
  if (!fld::which("wl-copy").empty()) {
    std::string out;
    fld::run_capture_input({"wl-copy", "--", o.picked}, "", out);
  } else {
    Fl::copy(o.picked.c_str(), (int)o.picked.size(), 1);
  }
  return 0;
}
