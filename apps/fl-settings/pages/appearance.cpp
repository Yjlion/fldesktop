// Appearance: theme, accent, fonts, icons, cursor, title bars, wallpaper
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <memory>
#include <set>

#include "fld/accents.h"
#include "fld/icons.h"
#include "fld/pickers.h"
#include "fld/styles.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

std::vector<std::string> theme_dirs(bool cursors) {
  std::set<std::string> names;
  std::vector<std::string> bases{home_dir() + "/.icons"};
  for (auto &d : data_dirs()) bases.push_back(path_join(d, "icons"));
  for (auto &b : bases)
    for (auto &n : list_dir(b)) {
      std::string p = path_join(b, n);
      bool has_cursors = is_dir(p + "/cursors");
      // icon themes: an index.theme and more than just cursors
      bool icon_theme = file_exists(p + "/index.theme") && n != "default" &&
                        !(has_cursors && list_dir(p).size() <= 3);
      if (cursors ? has_cursors : icon_theme) names.insert(n);
    }
  return std::vector<std::string>(names.begin(), names.end());
}

std::vector<std::string> font_families() {
  int n = Fl::set_fonts(nullptr);
  std::set<std::string> fams{"Sans", "Serif", "Monospace"};
  for (int i = 0; i < n; i++) {
    int attr;
    const char *name = Fl::get_font_name((Fl_Font)i, &attr);
    if (name && *name && !attr) fams.insert(name);
  }
  return std::vector<std::string>(fams.begin(), fams.end());
}

void fill_choice(Fl_Choice *c, const std::vector<std::string> &items, const std::string &cur) {
  int sel = 0;
  for (size_t i = 0; i < items.size(); i++) {
    std::string e;
    for (char ch : escape_at(items[i])) {
      if (ch == '/' || ch == '\\' || ch == '&' || ch == '_') e += '\\';
      e += ch;
    }
    c->add(e.c_str());
    if (items[i] == cur) sel = (int)i;
  }
  c->value(sel);
}

std::string choice_text(Fl_Choice *c, const std::vector<std::string> &items) {
  int i = c->value();
  return i >= 0 && i < (int)items.size() ? items[i] : "";
}

class AppearancePage : public Fl_Group {
public:
  ConfigFile desk{desktop_ini_path()}, wm{wm_ini_path()};
  Fl_Choice *theme, *style, *ui_font, *mono_font, *icons, *cursors, *titlebar, *panel_theme, *wp_mode;
  Fl_Spinner *radius, *ui_size, *mono_size, *cursor_size;
  AccentSwatches *accent;
  Fl_Check_Button *icons_follow;
  WallpaperGallery *gallery;
  std::string wallpaper;
  std::vector<std::string> fonts, icon_themes, cursor_themes;
  Fl_Color accent_color;

  AppearancePage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Appearance", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    f.heading("Style");
    f.row("Style:", 28, wx, wy, ww);
    theme = new Fl_Choice(wx, wy, 200, 28);
    theme->add("Light");
    theme->add("Dark");
    theme->value(lower(desk.ini.get("Appearance", "Theme", "light")) == "dark" ? 1 : 0);
    theme->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);
    f.row("Accent color:", 28, wx, wy, ww);
    accent = new AccentSwatches(wx, wy, AccentSwatches::preferred_width(28), 28);
    accent_color = palette().accent;
    accent->value(desk.ini.get("Appearance", "Accent", "#3584e4"));
    accent->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->accent_changed(); }, this);
    f.row("", 24, wx, wy, ww);
    icons_follow = new Fl_Check_Button(wx, wy, ww, 24, "Folder icons follow the accent color");
    icons_follow->tooltip("Switches to the fldesktop icon theme in the accent color");
    f.row("Widget style:", 28, wx, wy, ww);
    style = new Fl_Choice(wx, wy, 200, 28);
    std::string cur_style = normalize_style(desk.ini.get("Appearance", "Scheme", "gtk+"));
    for (size_t i = 0; i < widget_styles().size(); i++) {
      std::string n = widget_styles()[i].name;
      std::string e;
      for (char ch : n) {
        if (ch == '/' || ch == '\\' || ch == '&' || ch == '_' || ch == '+') e += '\\';
        e += ch;
      }
      style->add(e.c_str());
      if (cur_style == widget_styles()[i].id) style->value((int)i);
    }
    style->tooltip("How buttons, fields and frames are drawn");
    style->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->style_changed(); }, this);
    radius = new Fl_Spinner(wx + 290, wy, 60, 28, "Corners:");
    radius->range(0, 12);
    radius->value(desk.ini.get_int("Appearance", "Radius", 4));
    radius->tooltip("Corner radius of the Rounded style");
    radius->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->style_changed(); }, this);
    f.row("Preview:", 64, wx, wy, ww);
    {
      Fl_Group *pv = new Fl_Group(wx, wy, ww, 64);
      new Fl_Button(wx, wy, 90, 28, "Button");
      Fl_Button *def = new Fl_Button(wx + 98, wy, 90, 28, "Pressed");
      def->type(FL_TOGGLE_BUTTON);
      def->value(1);
      def->selection_color(fl_darker(palette().bg));
      Fl_Input *in = new Fl_Input(wx + 196, wy, std::max(80, ww - 196), 28);
      in->value("Text field");
      Fl_Check_Button *cb = new Fl_Check_Button(wx, wy + 34, 90, 26, "Check");
      cb->value(1);
      Fl_Round_Button *rb = new Fl_Round_Button(wx + 92, wy + 34, 80, 26, "Radio");
      rb->type(FL_RADIO_BUTTON);
      rb->value(1);
      Fl_Slider *sl = new Fl_Slider(wx + 176, wy + 37, 110, 20);
      sl->type(FL_HOR_NICE_SLIDER);
      sl->value(0.6);
      Fl_Progress *pr = new Fl_Progress(wx + 296, wy + 37, std::max(60, ww - 296), 20);
      pr->value(60);
      pr->selection_color(palette().accent);
      pv->end();
    }
    update_radius();
    f.row("Title bars:", 28, wx, wy, ww);
    titlebar = new Fl_Choice(wx, wy, 200, 28);
    titlebar->add("Match theme");
    titlebar->add("Dark");
    titlebar->add("Light");
    titlebar->add("Accent color");
    if (!wm.ini.has("appearance", "active_title")) titlebar->value(0);
    else {
      std::string at = lower(wm.ini.get("appearance", "active_title", ""));
      titlebar->value(at == "#2b2d31" ? 1 : at == "#f2f3f5" ? 2 : 3);
    }
    titlebar->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);
    // the panel can be dark with light windows, or the other way round
    f.row("Panel:", 28, wx, wy, ww);
    panel_theme = new Fl_Choice(wx, wy, 200, 28);
    panel_theme->add("Match theme");
    panel_theme->add("Dark");
    panel_theme->add("Light");
    std::string pt = lower(desk.ini.get("Appearance", "PanelTheme", "auto"));
    panel_theme->value(pt == "dark" ? 1 : pt == "light" ? 2 : 0);
    panel_theme->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);

    f.heading("Fonts");
    fonts = font_families();
    f.row("Interface:", 28, wx, wy, ww);
    ui_font = new Fl_Choice(wx, wy, ww - 90, 28);
    fill_choice(ui_font, fonts, desk.ini.get("Appearance", "Font", "Sans"));
    ui_font->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);
    ui_size = new Fl_Spinner(wx + ww - 80, wy, 80, 28);
    ui_size->range(8, 24);
    ui_size->value(desk.ini.get_int("Appearance", "FontSize", 14));
    ui_size->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);
    f.row("Monospace:", 28, wx, wy, ww);
    mono_font = new Fl_Choice(wx, wy, ww - 90, 28);
    fill_choice(mono_font, fonts, desk.ini.get("Appearance", "MonoFont", "Monospace"));
    mono_font->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);
    mono_size = new Fl_Spinner(wx + ww - 80, wy, 80, 28);
    mono_size->range(8, 28);
    mono_size->value(desk.ini.get_int("Appearance", "MonoFontSize", 14));
    mono_size->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);

    f.heading("Icons and Cursor");
    icon_themes = theme_dirs(false);
    f.row("Icon theme:", 28, wx, wy, ww);
    icons = new Fl_Choice(wx, wy, ww, 28);
    // the theme in effect: a stale IconTheme that disagrees with the accent is corrected
    std::string cur_icons = effective_icon_theme(desk.ini, [this](const std::string &t) {
      return std::find(icon_themes.begin(), icon_themes.end(), t) != icon_themes.end();
    });
    if (cur_icons.empty()) cur_icons = default_icon_theme();
    fill_choice(icons, icon_themes, cur_icons);
    icons->callback([](Fl_Widget *, void *d) {
      AppearancePage *p = (AppearancePage *)d;
      // a theme picked by hand that is not an accent theme ends "follow the accent"
      if (!is_accent_icon_theme(choice_text(p->icons, p->icon_themes))) p->icons_follow->value(0);
      p->save();
    }, this);
    icons_follow->value(desk.ini.get_bool("Appearance", "IconsFollowAccent", is_accent_icon_theme(cur_icons)));
    icons_follow->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->accent_changed(); }, this);
    cursor_themes = theme_dirs(true);
    f.row("Cursor:", 28, wx, wy, ww);
    cursors = new Fl_Choice(wx, wy, ww - 90, 28);
    fill_choice(cursors, cursor_themes, wm.ini.get("cursor", "theme", default_cursor_theme()));
    cursors->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);
    cursor_size = new Fl_Spinner(wx + ww - 80, wy, 80, 28);
    cursor_size->range(16, 96);
    cursor_size->step(8);
    cursor_size->value(wm.ini.get_int("cursor", "size", 24));
    cursor_size->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);

    f.heading("Wallpaper");
    f.row("", 96, wx, wy, ww);
    gallery = new WallpaperGallery(wx, wy, ww, 96);
    gallery->load(WallpaperGallery::default_dirs());
    gallery->callback([](Fl_Widget *, void *d) {
      AppearancePage *p = (AppearancePage *)d;
      p->wallpaper = p->gallery->value();
      p->save();
    }, this);
    f.row("", 28, wx, wy, ww);
    Fl_Button *choose = new Fl_Button(wx, wy, 140, 28, "Other Image…");
    choose->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->choose_wallpaper(); }, this);
    wp_mode = new Fl_Choice(wx + 214, wy, 140, 28, "Fit:");
    const char *modes[] = {"fill", "fit", "stretch", "center", "tile"};
    std::string cur_mode = desk.ini.get("Desktop", "WallpaperMode", "fill");
    for (int i = 0; i < 5; i++) {
      std::string m = modes[i];
      m[0] = (char)toupper((unsigned char)m[0]);
      wp_mode->add(m.c_str());
      if (cur_mode == modes[i]) wp_mode->value(i);
    }
    if (wp_mode->value() < 0) wp_mode->value(0);
    wp_mode->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);
    std::string wp = expand_tilde(desk.ini.get("Desktop", "Wallpaper", ""));
    if (wp.empty() || !file_exists(wp)) wp = fld_data_dir() + "/backgrounds/default.svg";
    wallpaper = wp;
    gallery->add_file(wp);
    gallery->value(wp);
    f.space(4);
    f.note("The panel and desktop change right away; other applications use the new style the next time they start.", 2);
    g->end();
    end();
  }

  void update_radius() {
    size_t i = (size_t)std::max(0, style->value());
    if (i < widget_styles().size() && std::string(widget_styles()[i].id) == "fld-rounded") radius->activate();
    else radius->deactivate();
  }

  // applied to Settings itself right away, as a live preview
  void style_changed() {
    size_t i = (size_t)std::max(0, style->value());
    if (i >= widget_styles().size()) return;
    update_radius();
    apply_style(widget_styles()[i].id, (int)radius->value());
    save();
  }

  // the accent picked a preset: follow it with the icon theme
  void accent_changed() {
    accent_color = parse_hex(accent->value());
    if (icons_follow->value()) {
      std::string want = accent_icon_theme(nearest_accent(accent->value()));
      for (size_t i = 0; i < icon_themes.size(); i++)
        if (icon_themes[i] == want) icons->value((int)i);
    }
    save();
  }

  static Fl_Color parse_hex(const std::string &h) {
    unsigned v = h.size() == 7 ? (unsigned)strtoul(h.c_str() + 1, nullptr, 16) : 0x3584e4;
    return fl_rgb_color((v >> 16) & 255, (v >> 8) & 255, v & 255);
  }

  void choose_wallpaper() {
    Fl_File_Chooser fc(dirname(wallpaper).c_str(), "Images (*.{png,jpg,jpeg,svg,gif,bmp,webp})",
                       Fl_File_Chooser::SINGLE, "Choose Wallpaper");
    fc.preview(1);
    fc.show();
    while (fc.shown()) Fl::wait();
    if (!fc.value()) return;
    wallpaper = fc.value();
    gallery->add_file(wallpaper);
    gallery->value(wallpaper);
    save();
  }

  void save() {
    desk.ini.load(desk.path); // pick up changes made elsewhere first
    wm.ini.load(wm.path);
    desk.ini.set("Appearance", "Theme", theme->value() == 1 ? "dark" : "light");
    const char *panel_themes[] = {"auto", "dark", "light"};
    desk.ini.set("Appearance", "PanelTheme", panel_themes[std::max(0, std::min(2, panel_theme->value()))]);
    uchar r, g, b;
    Fl::get_color(accent_color, r, g, b);
    char hex[8];
    snprintf(hex, sizeof hex, "#%02x%02x%02x", r, g, b);
    desk.ini.set("Appearance", "Accent", hex);
    size_t si = (size_t)std::max(0, style->value());
    if (si < widget_styles().size()) desk.ini.set("Appearance", "Scheme", widget_styles()[si].id);
    desk.ini.set_int("Appearance", "Radius", (int)radius->value());
    desk.ini.set("Appearance", "Font", choice_text(ui_font, fonts));
    desk.ini.set_int("Appearance", "FontSize", (int)ui_size->value());
    desk.ini.set("Appearance", "MonoFont", choice_text(mono_font, fonts));
    desk.ini.set_int("Appearance", "MonoFontSize", (int)mono_size->value());
    std::string it = choice_text(icons, icon_themes);
    // the monochrome icons come in a light and a dark version
    bool dark = theme->value() == 1;
    if (dark && it == "fldesktop-mono") it = "fldesktop-mono-dark";
    if (!dark && it == "fldesktop-mono-dark") it = "fldesktop-mono";
    for (size_t i = 0; i < icon_themes.size(); i++)
      if (icon_themes[i] == it) icons->value((int)i);
    desk.ini.set("General", "IconTheme", it);
    desk.ini.set_bool("Appearance", "IconsFollowAccent", icons_follow->value() != 0);
    desk.ini.set("Desktop", "Wallpaper", wallpaper);
    const char *modes[] = {"fill", "fit", "stretch", "center", "tile"};
    desk.ini.set("Desktop", "WallpaperMode", modes[std::max(0, wp_mode->value())]);
    desk.save();
    wm.ini.set("cursor", "theme", choice_text(cursors, cursor_themes));
    wm.ini.set_int("cursor", "size", (int)cursor_size->value());
    switch (titlebar->value()) {
      case 0:
        // match the theme: fl-wm picks the colours from desktop.ini
        for (const char *k : {"active_title", "active_text", "inactive_title", "inactive_text"})
          wm.ini.remove("appearance", k);
        break;
      case 2:
        wm.ini.set("appearance", "active_title", "#f2f3f5");
        wm.ini.set("appearance", "active_text", "#222428");
        wm.ini.set("appearance", "inactive_title", "#e3e5e8");
        wm.ini.set("appearance", "inactive_text", "#8a8f98");
        break;
      case 3:
        wm.ini.set("appearance", "active_title", hex);
        wm.ini.set("appearance", "active_text", "#ffffff");
        wm.ini.set("appearance", "inactive_title", "#e8e9eb");
        wm.ini.set("appearance", "inactive_text", "#6b6f76");
        break;
      default:
        wm.ini.set("appearance", "active_title", "#2b2d31");
        wm.ini.set("appearance", "active_text", "#ffffff");
        wm.ini.set("appearance", "inactive_title", "#e8e9eb");
        wm.ini.set("appearance", "inactive_text", "#6b6f76");
    }
    save_wm(wm);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new AppearancePage(x, y, w, h); }

} // namespace

PageInfo page_appearance() {
  return {"appearance", "Appearance", "preferences-desktop-theme", "Personal",
          "theme dark light accent color style scheme widgets buttons rounded flat font icons cursor wallpaper background title bar panel taskbar", create};
}

} // namespace fld
