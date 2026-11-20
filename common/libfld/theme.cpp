#include "fld/theme.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/styles.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <cstdlib>

namespace fld {

static Palette g_pal;
static Fl_Font g_mono = FL_COURIER;
static int g_mono_size = 14, g_ui_size = 14;

static Fl_Color parse_color(const std::string &s, Fl_Color def) {
  if (s.size() == 7 && s[0] == '#') {
    unsigned v = (unsigned)strtoul(s.c_str() + 1, nullptr, 16);
    return fl_rgb_color((v >> 16) & 255, (v >> 8) & 255, v & 255);
  }
  return def;
}

static std::string &config_override() {
  static std::string p;
  return p;
}

void set_desktop_config_path(const std::string &path) { config_override() = path; }

std::string desktop_config_path() {
  if (!config_override().empty()) return config_override();
  return fld_config_dir() + "/desktop.ini";
}

void init_app(const char *app_id, int argc, char **argv) {
  Ini cfg;
  cfg.load(desktop_config_path());
  const std::string A = "Appearance";
  bool dark = lower(cfg.get(A, "Theme", "light")) == "dark";
  // Accessibility: Contrast=high for black and white with strong borders
  bool high = lower(cfg.get(A, "Contrast", "normal")) == "high";
  Palette &p = g_pal;
  p.dark = dark;
  p.high_contrast = high;
  if (high && dark) {
    p.bg = FL_BLACK;
    p.bg2 = FL_BLACK;
    p.fg = FL_WHITE;
    p.fg_dim = fl_rgb_color(0xdd, 0xdd, 0xdd);
    p.border = FL_WHITE;
    p.panel_bg = FL_BLACK;
    p.panel_fg = FL_WHITE;
  } else if (high) {
    p.bg = FL_WHITE;
    p.bg2 = FL_WHITE;
    p.fg = FL_BLACK;
    p.fg_dim = fl_rgb_color(0x22, 0x22, 0x22);
    p.border = FL_BLACK;
    p.panel_bg = FL_BLACK;
    p.panel_fg = FL_WHITE;
  } else if (dark) {
    p.bg = fl_rgb_color(0x2b, 0x2d, 0x31);
    p.bg2 = fl_rgb_color(0x1e, 0x1f, 0x22);
    p.fg = fl_rgb_color(0xe6, 0xe6, 0xe6);
    p.fg_dim = fl_rgb_color(0x9a, 0x9d, 0xa3);
    p.border = fl_rgb_color(0x45, 0x48, 0x4e);
    p.panel_bg = fl_rgb_color(0x18, 0x19, 0x1c);
    p.panel_fg = fl_rgb_color(0xee, 0xee, 0xee);
  } else {
    p.bg = fl_rgb_color(0xee, 0xef, 0xf1);
    p.bg2 = fl_rgb_color(0xff, 0xff, 0xff);
    p.fg = fl_rgb_color(0x22, 0x24, 0x28);
    p.fg_dim = fl_rgb_color(0x6b, 0x6f, 0x76);
    p.border = fl_rgb_color(0xc4, 0xc7, 0xcc);
    p.panel_bg = fl_rgb_color(0x23, 0x25, 0x2a);
    p.panel_fg = fl_rgb_color(0xf0, 0xf0, 0xf0);
  }
  p.accent = parse_color(cfg.get(A, "Accent", "#3584e4"), fl_rgb_color(0x35, 0x84, 0xe4));
  // a dark accent on white, a light one on black, so text on it stays readable
  if (high) p.accent = dark ? fl_rgb_color(0xff, 0xd7, 0x00) : fl_rgb_color(0x00, 0x3d, 0xa5);
  p.accent_fg = high && dark ? FL_BLACK : FL_WHITE;
  p.selection = p.accent;
  p.danger = fl_rgb_color(0xe0, 0x1b, 0x24);

  unsigned char r, g, b;
  Fl::get_color(p.bg, r, g, b);
  Fl::background(r, g, b);
  Fl::get_color(p.bg2, r, g, b);
  Fl::background2(r, g, b);
  Fl::get_color(p.fg, r, g, b);
  Fl::foreground(r, g, b);
  Fl::set_color(FL_SELECTION_COLOR, p.selection);
  // high contrast: flat boxes framed in the border colour (styles.cpp)
  if (high) apply_style("fld-flat", 0);
  else apply_style(cfg.get(A, "Scheme", "gtk+"), cfg.get_int(A, "Radius", 4));
  Fl::set_color(FL_INACTIVE_COLOR, p.fg_dim);
  Fl_Tooltip::color(dark ? fl_rgb_color(0x10, 0x10, 0x12) : fl_rgb_color(0x30, 0x32, 0x36));
  Fl_Tooltip::textcolor(FL_WHITE);
  Fl_Tooltip::size(12);

  std::string ui_font = cfg.get(A, "Font", "Sans");
  std::string mono = cfg.get(A, "MonoFont", "Monospace");
  // Accessibility: TextScale (1.0 .. 2.0) makes all text larger
  double scale = cfg.get_double(A, "TextScale", 1.0);
  if (scale < 1.0 || scale > 2.0) scale = 1.0;
  g_ui_size = (int)(cfg.get_int(A, "FontSize", 14) * scale + 0.5);
  g_mono_size = (int)(cfg.get_int(A, "MonoFontSize", 14) * scale + 0.5);
  // FL_HELVETICA family -> UI font, FL_COURIER family -> monospace font
  static std::string uf, ufb, ufi, ufbi, mf, mfb, mfi, mfbi;
  uf = ui_font; ufb = ui_font + " Bold"; ufi = ui_font + " Italic"; ufbi = ui_font + " Bold Italic";
  mf = mono; mfb = mono + " Bold"; mfi = mono + " Italic"; mfbi = mono + " Bold Italic";
  Fl::set_font(FL_HELVETICA, uf.c_str());
  Fl::set_font(FL_HELVETICA_BOLD, ufb.c_str());
  Fl::set_font(FL_HELVETICA_ITALIC, ufi.c_str());
  Fl::set_font(FL_HELVETICA_BOLD_ITALIC, ufbi.c_str());
  Fl::set_font(FL_COURIER, mf.c_str());
  Fl::set_font(FL_COURIER_BOLD, mfb.c_str());
  Fl::set_font(FL_COURIER_ITALIC, mfi.c_str());
  Fl::set_font(FL_COURIER_BOLD_ITALIC, mfbi.c_str());
  g_mono = FL_COURIER;
  FL_NORMAL_SIZE = g_ui_size;
  fl_message_font(FL_HELVETICA, g_ui_size);

  if (app_id) Fl_Window::default_xclass(app_id);
  Fl::visual(FL_RGB);
  install_edit_menu(); // Cut/Copy/Paste on a right click in every text field
  (void)argc;
  (void)argv;
}

const Palette &palette() { return g_pal; }
Fl_Font mono_font() { return g_mono; }
int mono_size() { return g_mono_size; }
int ui_font_size() { return g_ui_size; }

void set_window_icon(Fl_Window *w, const char *icon_name) {
  std::string p = icon_path(icon_name, 64);
  if (p.empty()) return;
  Fl_Image *img = load_image(p);
  if (!img) return;
  Fl_RGB_Image *rgb = dynamic_cast<Fl_RGB_Image *>(img);
  if (rgb) w->icon(rgb);
  delete img;
}

} // namespace fld
