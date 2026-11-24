#include "fld/styles.h"
#include "fld/theme.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <algorithm>

namespace fld {

const std::vector<WidgetStyle> &widget_styles() {
  static const std::vector<WidgetStyle> s = {
      {"gtk+", "GTK+ (default)", true},  {"gleam", "Gleam", true},       {"plastic", "Plastic", true},
      {"oxy", "Oxy", true},              {"none", "FLTK classic", true}, {"fld-flat", "Flat", false},
      {"fld-rounded", "Rounded", false}, {"fld-classic", "Retro bevel", false},
  };
  return s;
}

std::string normalize_style(const std::string &id_in) {
  std::string id = lower(trim(id_in));
  if (id == "base" || id.empty()) id = "none";
  for (auto &s : widget_styles())
    if (id == s.id) return id;
  return "gtk+";
}

namespace {

std::string g_style = "gtk+";
int g_radius = 4;

Fl_Color border_for(Fl_Color c) {
  if (palette().high_contrast) return palette().border;
  // a border a bit darker than the fill, never lighter than the theme border
  return fl_color_average(c, palette().dark ? FL_BLACK : fl_rgb_color(0x60, 0x64, 0x6a), 0.55f);
}

// ---- flat: 1px border around a plain fill --------------------------------------------

void flat_frame(int x, int y, int w, int h, Fl_Color c) {
  fl_color(border_for(c));
  fl_rect(x, y, w, h);
}
void flat_box(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_rectf(x + 1, y + 1, w - 2, h - 2);
  flat_frame(x, y, w, h, c);
}
void flat_down_box(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_rectf(x + 1, y + 1, w - 2, h - 2);
  fl_color(fl_color_average(border_for(c), FL_BLACK, 0.85f));
  fl_rect(x, y, w, h);
}
void flat_down_frame(int x, int y, int w, int h, Fl_Color c) {
  fl_color(fl_color_average(border_for(c), FL_BLACK, 0.85f));
  fl_rect(x, y, w, h);
}

// ---- rounded --------------------------------------------------------------------

int radius_for(int w, int h) { return std::max(0, std::min(g_radius, std::min(w, h) / 2 - 1)); }

void rounded_frame(int x, int y, int w, int h, Fl_Color c) {
  fl_color(border_for(c));
  fl_rounded_rect(x, y, w, h, radius_for(w, h));
}
void rounded_box(int x, int y, int w, int h, Fl_Color c) {
  int r = radius_for(w, h);
  fl_color(c);
  fl_rounded_rectf(x, y, w, h, r);
  // a soft highlight on the top half of raised boxes
  if (h > 8) {
    fl_color(fl_color_average(c, FL_WHITE, 0.88f));
    fl_rounded_rectf(x + 1, y + 1, w - 2, h / 2, std::max(0, r - 1));
    fl_color(c);
    fl_rectf(x + 1, y + h / 2 - std::max(1, r), w - 2, std::max(1, r));
  }
  rounded_frame(x, y, w, h, c);
}
void rounded_down_box(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_rounded_rectf(x, y, w, h, radius_for(w, h));
  fl_color(fl_color_average(border_for(c), FL_BLACK, 0.85f));
  fl_rounded_rect(x, y, w, h, radius_for(w, h));
}
void rounded_down_frame(int x, int y, int w, int h, Fl_Color c) {
  fl_color(fl_color_average(border_for(c), FL_BLACK, 0.85f));
  fl_rounded_rect(x, y, w, h, radius_for(w, h));
}
void round_up(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_pie(x, y, w, h, 0, 360);
  fl_color(border_for(c));
  fl_arc(x, y, w, h, 0, 360);
}
void round_down(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_pie(x, y, w, h, 0, 360);
  fl_color(fl_color_average(border_for(c), FL_BLACK, 0.85f));
  fl_arc(x, y, w, h, 0, 360);
}

// ---- classic bevel (two-pixel 3D edges) -------------------------------------------------

void bevel(int x, int y, int w, int h, Fl_Color c, bool up, bool thin) {
  Fl_Color hi = fl_color_average(c, FL_WHITE, 0.25f), lo = fl_color_average(c, FL_BLACK, 0.55f);
  Fl_Color hi2 = fl_color_average(c, FL_WHITE, 0.7f), lo2 = fl_color_average(c, FL_BLACK, 0.8f);
  if (!up) {
    std::swap(hi, lo);
    std::swap(hi2, lo2);
  }
  // outer
  fl_color(up ? hi2 : fl_color_average(c, FL_BLACK, 0.55f));
  fl_xyline(x, y, x + w - 1);
  fl_yxline(x, y, y + h - 1);
  fl_color(up ? fl_color_average(c, FL_BLACK, 0.3f) : hi2);
  fl_xyline(x, y + h - 1, x + w - 1);
  fl_yxline(x + w - 1, y, y + h - 1);
  if (thin) return;
  // inner
  fl_color(up ? hi : lo2);
  fl_xyline(x + 1, y + 1, x + w - 2);
  fl_yxline(x + 1, y + 1, y + h - 2);
  fl_color(up ? lo2 : hi);
  fl_xyline(x + 1, y + h - 2, x + w - 2);
  fl_yxline(x + w - 2, y + 1, y + h - 2);
}
void classic_up_frame(int x, int y, int w, int h, Fl_Color c) { bevel(x, y, w, h, c, true, false); }
void classic_down_frame(int x, int y, int w, int h, Fl_Color c) { bevel(x, y, w, h, c, false, false); }
void classic_thin_up_frame(int x, int y, int w, int h, Fl_Color c) { bevel(x, y, w, h, c, true, true); }
void classic_thin_down_frame(int x, int y, int w, int h, Fl_Color c) { bevel(x, y, w, h, c, false, true); }
void classic_up_box(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_rectf(x + 2, y + 2, w - 4, h - 4);
  bevel(x, y, w, h, c, true, false);
}
void classic_down_box(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_rectf(x + 2, y + 2, w - 4, h - 4);
  bevel(x, y, w, h, c, false, false);
}
void classic_thin_up_box(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_rectf(x + 1, y + 1, w - 2, h - 2);
  bevel(x, y, w, h, c, true, true);
}
void classic_thin_down_box(int x, int y, int w, int h, Fl_Color c) {
  fl_color(c);
  fl_rectf(x + 1, y + 1, w - 2, h - 2);
  bevel(x, y, w, h, c, false, true);
}

} // namespace

const std::string &current_style() { return g_style; }

void apply_style(const std::string &id_in, int radius) {
  std::string id = normalize_style(id_in);
  g_style = id;
  g_radius = std::max(0, std::min(16, radius));
  bool builtin = true;
  for (auto &s : widget_styles())
    if (id == s.id) builtin = s.builtin;
  // the built-in scheme resets all boxtypes; ours are drawn on top of "base"
  Fl::scheme(builtin ? (id == "none" ? "base" : id.c_str()) : "base");
  if (id == "fld-flat") {
    Fl::set_boxtype(FL_UP_BOX, flat_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_DOWN_BOX, flat_down_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_UP_BOX, flat_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_DOWN_BOX, flat_down_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_UP_FRAME, flat_frame, 1, 1, 2, 2);
    Fl::set_boxtype(FL_DOWN_FRAME, flat_down_frame, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_UP_FRAME, flat_frame, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_DOWN_FRAME, flat_down_frame, 1, 1, 2, 2);
    Fl::set_boxtype(FL_ROUND_UP_BOX, round_up, 1, 1, 2, 2);
    Fl::set_boxtype(FL_ROUND_DOWN_BOX, round_down, 1, 1, 2, 2);
  } else if (id == "fld-rounded") {
    Fl::set_boxtype(FL_UP_BOX, rounded_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_DOWN_BOX, rounded_down_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_UP_BOX, rounded_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_DOWN_BOX, rounded_down_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_UP_FRAME, rounded_frame, 1, 1, 2, 2);
    Fl::set_boxtype(FL_DOWN_FRAME, rounded_down_frame, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_UP_FRAME, rounded_frame, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_DOWN_FRAME, rounded_down_frame, 1, 1, 2, 2);
    Fl::set_boxtype(FL_ROUND_UP_BOX, round_up, 1, 1, 2, 2);
    Fl::set_boxtype(FL_ROUND_DOWN_BOX, round_down, 1, 1, 2, 2);
  } else if (id == "fld-classic") {
    Fl::set_boxtype(FL_UP_BOX, classic_up_box, 2, 2, 4, 4);
    Fl::set_boxtype(FL_DOWN_BOX, classic_down_box, 2, 2, 4, 4);
    Fl::set_boxtype(FL_THIN_UP_BOX, classic_thin_up_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_DOWN_BOX, classic_thin_down_box, 1, 1, 2, 2);
    Fl::set_boxtype(FL_UP_FRAME, classic_up_frame, 2, 2, 4, 4);
    Fl::set_boxtype(FL_DOWN_FRAME, classic_down_frame, 2, 2, 4, 4);
    Fl::set_boxtype(FL_THIN_UP_FRAME, classic_thin_up_frame, 1, 1, 2, 2);
    Fl::set_boxtype(FL_THIN_DOWN_FRAME, classic_thin_down_frame, 1, 1, 2, 2);
  }
  // flat styles: slimmer scrollbars (their arrows take less from every list)
  Fl::scrollbar_size(id == "fld-flat" || id == "fld-rounded" ? 12 : 16);
  // redraw everything that is already on screen
  for (Fl_Window *w = Fl::first_window(); w; w = Fl::next_window(w)) w->redraw();
}

} // namespace fld
