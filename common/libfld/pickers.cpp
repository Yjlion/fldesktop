#include "fld/i18n.h"
#include "fld/pickers.h"
#include "fld/menus.h"
#include "fld/accents.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Tooltip.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdio>

namespace fld {

static Fl_Color hex_color(const std::string &h) {
  if (h.size() != 7 || h[0] != '#') return FL_GRAY;
  unsigned v = (unsigned)strtoul(h.c_str() + 1, nullptr, 16);
  return fl_rgb_color((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

// ---- AccentSwatches --------------------------------------------------------

static const int kGap = 8;

AccentSwatches::AccentSwatches(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
  box(FL_NO_BOX);
}

int AccentSwatches::preferred_width(int h) {
  return ((int)accent_presets().size() + 1) * (h + kGap);
}

void AccentSwatches::value(const std::string &hex) {
  value_ = lower(trim(hex));
  if (accent_index(value_) < 0) custom_ = value_;
  redraw();
}

int AccentSwatches::preset() const { return accent_index(value_); }

void AccentSwatches::swatch_box(int i, int &cx, int &cy, int &r) const {
  int d = h();
  r = d / 2 - 3;
  cx = x() + i * (d + kGap) + d / 2;
  cy = y() + d / 2;
}

int AccentSwatches::swatch_at(int mx, int my) const {
  int n = (int)accent_presets().size() + 1;
  for (int i = 0; i < n; i++) {
    int cx, cy, r;
    swatch_box(i, cx, cy, r);
    int dx = mx - cx, dy = my - cy;
    if (dx * dx + dy * dy <= (r + 3) * (r + 3)) return i;
  }
  return -1;
}

void AccentSwatches::draw() {
  const auto &p = accent_presets();
  int n = (int)p.size() + 1;
  int sel = preset();
  if (sel < 0) sel = (int)p.size();
  for (int i = 0; i < n; i++) {
    int cx, cy, r;
    swatch_box(i, cx, cy, r);
    bool custom = i == (int)p.size();
    if (i == sel) {  // selection ring
      fl_color(palette().fg);
      fl_line_style(FL_SOLID, 2);
      fl_arc(cx - r - 3, cy - r - 3, 2 * r + 6, 2 * r + 6, 0, 360);
      fl_line_style(0);
    } else if (i == hover_) {
      fl_color(palette().fg_dim);
      fl_arc(cx - r - 2, cy - r - 2, 2 * r + 4, 2 * r + 4, 0, 360);
    }
    if (!custom) {
      fl_color(hex_color(p[i].hex));
      fl_pie(cx - r, cy - r, 2 * r, 2 * r, 0, 360);
    } else if (!custom_.empty()) {
      fl_color(hex_color(custom_));
      fl_pie(cx - r, cy - r, 2 * r, 2 * r, 0, 360);
    } else {  // a color wheel
      static const char *wheel[] = {"#e62d42", "#ed5b00", "#f5c211", "#3a944a", "#2190a4", "#3584e4", "#9141ac", "#d56199"};
      for (int k = 0; k < 8; k++) {
        fl_color(hex_color(wheel[k]));
        fl_pie(cx - r, cy - r, 2 * r, 2 * r, k * 45, (k + 1) * 45);
      }
    }
    if (custom) {  // a "+" mark on the custom swatch
      fl_color(FL_WHITE);
      fl_line_style(FL_SOLID | FL_CAP_ROUND, 2);
      int m = std::max(3, r / 2);
      fl_line(cx - m, cy, cx + m, cy);
      fl_line(cx, cy - m, cx, cy + m);
      fl_line_style(0);
    } else if (i == sel) {  // a check mark
      fl_color(FL_WHITE);
      fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
      int m = std::max(3, r / 2);
      fl_begin_line();
      fl_vertex(cx - m, cy);
      fl_vertex(cx - m / 3, cy + m * 2 / 3);
      fl_vertex(cx + m, cy - m / 2);
      fl_end_line();
      fl_line_style(0);
    }
  }
}

int AccentSwatches::handle(int e) {
  const auto &p = accent_presets();
  switch (e) {
    case FL_ENTER:
    case FL_MOVE: {
      int i = swatch_at(Fl::event_x(), Fl::event_y());
      if (i != hover_) {
        hover_ = i;
        if (i >= 0) {
          static std::string tip;
          tip = i < (int)p.size() ? p[i].name : "Custom color…";
          Fl_Tooltip::enter_area(this, 0, 0, w(), h(), tip.c_str());
        }
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover_ = -1;
      redraw();
      return 1;
    case FL_PUSH:
      return swatch_at(Fl::event_x(), Fl::event_y()) >= 0 ? 1 : 0;
    case FL_RELEASE: {
      int i = swatch_at(Fl::event_x(), Fl::event_y());
      if (i < 0) return 1;
      if (i < (int)p.size()) {
        value_ = p[i].hex;
      } else {
        Fl_Color c = hex_color(custom_.empty() ? value_ : custom_);
        uchar r, g, b;
        Fl::get_color(c, r, g, b);
        if (!fl_color_chooser("Accent color", r, g, b)) return 1;
        char hex[8];
        snprintf(hex, sizeof hex, "#%02x%02x%02x", r, g, b);
        value_ = hex;
        if (accent_index(value_) < 0) custom_ = value_;
      }
      redraw();
      do_callback();
      return 1;
    }
    default: return Fl_Widget::handle(e);
  }
}

// ---- WallpaperGallery -------------------------------------------------------

static const int kThumbW = 128, kThumbH = 72;

class WallpaperGallery::Thumb : public Fl_Widget {
public:
  std::string path;
  Fl_Image *img = nullptr;
  bool tried = false, hover = false;
  WallpaperGallery *gal;
  Thumb(WallpaperGallery *g, const std::string &p) : Fl_Widget(0, 0, kThumbW + 8, kThumbH + 8), path(p), gal(g) {
    copy_tooltip(basename(p).c_str());
  }
  ~Thumb() override { delete img; }
  void draw() override {
    if (!tried) {  // loaded on first draw: SVG wallpapers take a moment
      tried = true;
      img = load_image(path);
      if (img) {
        // fill the thumbnail, cropping the longer side
        double ar = (double)img->w() / std::max(1, img->h()), tr = (double)kThumbW / kThumbH;
        int sw = ar > tr ? (int)(kThumbH * ar) : kThumbW, sh = ar > tr ? kThumbH : (int)(kThumbW / ar);
        if (extension(path) == "svg") img->scale(sw, sh, 0, 1);
        else {
          Fl_Image *c = img->copy(sw, sh);
          delete img;
          img = c;
        }
      }
    }
    bool sel = gal->value_ == path;
    fl_color(parent()->color());
    fl_rectf(x(), y(), w(), h());
    if (sel || hover) {
      fl_color(sel ? palette().accent : palette().border);
      fl_rounded_rectf(x(), y(), w(), h(), 6);
    }
    int ix = x() + 4, iy = y() + 4;
    fl_push_clip(ix, iy, kThumbW, kThumbH);
    if (img) img->draw(ix - (img->w() - kThumbW) / 2, iy - (img->h() - kThumbH) / 2);
    else {
      fl_color(palette().bg);
      fl_rectf(ix, iy, kThumbW, kThumbH);
    }
    fl_pop_clip();
  }
  int handle(int e) override {
    switch (e) {
      case FL_ENTER: hover = true; redraw(); return 1;
      case FL_LEAVE: hover = false; redraw(); return 1;
      case FL_PUSH:
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          WallpaperGallery *g = gal;
          std::string p = path;
          Menu m;
          m.item(_("Set as Wallpaper"), [g, p] {
            g->value(p);
            g->do_callback();
          });
          m.item(_("Open in Image Viewer"), [p] { spawn({"fl-view", p}); });
          m.item(_("Show in Files"), [p] { spawn({"fl-files", dirname(p)}); });
          m.item(_("Copy Path"), [p] { Fl::copy(p.c_str(), (int)p.size(), 1); });
          m.popup();
        }
        return 1;
      case FL_RELEASE:
        if (Fl::event_button() != FL_LEFT_MOUSE) return 1;
        if (Fl::event_inside(this)) {
          gal->value(path);
          gal->do_callback();
        }
        return 1;
      default: return Fl_Widget::handle(e);
    }
  }
};

WallpaperGallery::WallpaperGallery(int x, int y, int w, int h) : Fl_Scroll(x, y, w, h) {
  type(Fl_Scroll::HORIZONTAL);
  box(FL_FLAT_BOX);
  color(palette().bg2);
  scrollbar_size(10);
  end();
}

WallpaperGallery::~WallpaperGallery() = default;

std::vector<std::string> WallpaperGallery::default_dirs() {
  std::vector<std::string> d{data_home() + "/backgrounds", fld_data_dir() + "/backgrounds"};
  return d;
}

static bool image_file(const std::string &p) {
  std::string e = extension(p);
  return e == "svg" || e == "png" || e == "jpg" || e == "jpeg" || e == "webp" || e == "bmp" || e == "gif";
}

void WallpaperGallery::load(const std::vector<std::string> &dirs) {
  for (auto *t : thumbs_) {
    remove(t);
    delete t;
  }
  thumbs_.clear();
  std::vector<std::string> files;
  for (auto &d : dirs) {
    std::vector<std::string> names = list_dir(d);
    std::sort(names.begin(), names.end());
    for (auto &n : names) {
      std::string p = path_join(d, n);
      if (image_file(p) && !is_dir(p) && std::find(files.begin(), files.end(), p) == files.end()) files.push_back(p);
    }
  }
  begin();
  for (auto &f : files) thumbs_.push_back(new Thumb(this, f));
  end();
  relayout();
}

void WallpaperGallery::add_file(const std::string &path) {
  for (auto *t : thumbs_)
    if (t->path == path) return;
  begin();
  Thumb *t = new Thumb(this, path);
  end();
  thumbs_.insert(thumbs_.begin(), t);
  relayout();
}

void WallpaperGallery::relayout() {
  int xx = x() + 2;
  for (auto *t : thumbs_) {
    t->resize(xx, y() + 2, kThumbW + 8, kThumbH + 8);
    xx += kThumbW + 12;
  }
  scroll_to(0, 0);
  redraw();
}

void WallpaperGallery::value(const std::string &path) {
  value_ = path;
  for (auto *t : thumbs_)
    if (t->path == path) {  // bring it into view
      int tx = t->x() - x() + xposition();
      if (tx < xposition() || tx + t->w() > xposition() + w()) scroll_to(std::max(0, tx - 8), 0);
    }
  redraw();
}

} // namespace fld
