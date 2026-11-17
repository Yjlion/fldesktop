// Appearance: theme, accent, fonts, icons, cursor, title bars, wallpaper
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_draw.H>

#include <memory>
#include <set>

#include "fld/icons.h"
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

class WallpaperPreview : public Fl_Widget {
public:
  std::unique_ptr<Fl_Image> img;
  std::string path;
  WallpaperPreview(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void set(const std::string &p) {
    path = p;
    img.reset();
    std::unique_ptr<Fl_Image> src(load_image(p));
    if (src) {
      double s = std::max((double)w() / src->w(), (double)h() / src->h());
      img.reset(src->copy((int)(src->w() * s) + 1, (int)(src->h() * s) + 1));
    }
    redraw();
  }
  void draw() override {
    fl_push_clip(x(), y(), w(), h());
    fl_color(fl_rgb_color(0x2d, 0x3a, 0x4f));
    fl_rectf(x(), y(), w(), h());
    if (img) img->draw(x() + (w() - img->w()) / 2, y() + (h() - img->h()) / 2);
    fl_pop_clip();
    fl_color(palette().border);
    fl_rect(x(), y(), w(), h());
  }
};

class AppearancePage : public Fl_Group {
public:
  ConfigFile desk{desktop_ini_path()}, wm{wm_ini_path()};
  Fl_Choice *theme, *ui_font, *mono_font, *icons, *cursors, *titlebar, *wp_mode;
  Fl_Spinner *ui_size, *mono_size, *cursor_size;
  Fl_Button *accent;
  WallpaperPreview *preview;
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
    accent = new Fl_Button(wx, wy, 120, 28);
    accent_color = palette().accent;
    accent->color(accent_color);
    accent->tooltip("Choose the accent color");
    accent->callback([](Fl_Widget *, void *d) {
      AppearancePage *p = (AppearancePage *)d;
      uchar r, g, b;
      Fl::get_color(p->accent_color, r, g, b);
      if (fl_color_chooser("Accent color", r, g, b)) {
        p->accent_color = fl_rgb_color(r, g, b);
        p->accent->color(p->accent_color);
        p->accent->redraw();
        p->save();
      }
    }, this);
    f.row("Title bars:", 28, wx, wy, ww);
    titlebar = new Fl_Choice(wx, wy, 200, 28);
    titlebar->add("Dark");
    titlebar->add("Light");
    titlebar->add("Accent color");
    std::string at = lower(wm.ini.get("appearance", "active_title", "#2b2d31"));
    titlebar->value(at == "#2b2d31" ? 0 : at == "#f2f3f5" ? 1 : 2);
    titlebar->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);

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
    fill_choice(icons, icon_themes, desk.ini.get("General", "IconTheme", "Adwaita"));
    icons->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);
    cursor_themes = theme_dirs(true);
    f.row("Cursor:", 28, wx, wy, ww);
    cursors = new Fl_Choice(wx, wy, ww - 90, 28);
    fill_choice(cursors, cursor_themes, wm.ini.get("cursor", "theme", "Adwaita"));
    cursors->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);
    cursor_size = new Fl_Spinner(wx + ww - 80, wy, 80, 28);
    cursor_size->range(16, 96);
    cursor_size->step(8);
    cursor_size->value(wm.ini.get_int("cursor", "size", 24));
    cursor_size->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->save(); }, this);

    f.heading("Wallpaper");
    f.row("", 120, wx, wy, ww);
    preview = new WallpaperPreview(wx, wy, 200, 120);
    Fl_Button *choose = new Fl_Button(wx + 214, wy, 140, 28, "Choose…");
    choose->callback([](Fl_Widget *, void *d) { ((AppearancePage *)d)->choose_wallpaper(); }, this);
    wp_mode = new Fl_Choice(wx + 214, wy + 40, 140, 28);
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
    preview->set(wp);
    f.space(4);
    f.note("The panel and desktop change right away; other applications use the new style the next time they start.", 2);
    g->end();
    end();
  }

  void choose_wallpaper() {
    Fl_File_Chooser fc(dirname(preview->path).c_str(), "Images (*.{png,jpg,jpeg,svg,gif,bmp,webp})",
                       Fl_File_Chooser::SINGLE, "Choose Wallpaper");
    fc.preview(1);
    fc.show();
    while (fc.shown()) Fl::wait();
    if (!fc.value()) return;
    preview->set(fc.value());
    save();
  }

  void save() {
    desk.ini.load(desk.path); // pick up changes made elsewhere first
    wm.ini.load(wm.path);
    desk.ini.set("Appearance", "Theme", theme->value() == 1 ? "dark" : "light");
    uchar r, g, b;
    Fl::get_color(accent_color, r, g, b);
    char hex[8];
    snprintf(hex, sizeof hex, "#%02x%02x%02x", r, g, b);
    desk.ini.set("Appearance", "Accent", hex);
    desk.ini.set("Appearance", "Font", choice_text(ui_font, fonts));
    desk.ini.set_int("Appearance", "FontSize", (int)ui_size->value());
    desk.ini.set("Appearance", "MonoFont", choice_text(mono_font, fonts));
    desk.ini.set_int("Appearance", "MonoFontSize", (int)mono_size->value());
    desk.ini.set("General", "IconTheme", choice_text(icons, icon_themes));
    desk.ini.set("Desktop", "Wallpaper", preview->path);
    const char *modes[] = {"fill", "fit", "stretch", "center", "tile"};
    desk.ini.set("Desktop", "WallpaperMode", modes[std::max(0, wp_mode->value())]);
    desk.save();
    wm.ini.set("cursor", "theme", choice_text(cursors, cursor_themes));
    wm.ini.set_int("cursor", "size", (int)cursor_size->value());
    switch (titlebar->value()) {
      case 1:
        wm.ini.set("appearance", "active_title", "#f2f3f5");
        wm.ini.set("appearance", "active_text", "#222428");
        wm.ini.set("appearance", "inactive_title", "#e3e5e8");
        wm.ini.set("appearance", "inactive_text", "#8a8f98");
        break;
      case 2:
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
          "theme dark light accent color font icons cursor wallpaper background title bar", create};
}

} // namespace fld
