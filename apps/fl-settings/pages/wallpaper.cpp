// Wallpaper: the desktop's picture ([Desktop] Wallpaper and WallpaperMode in
// desktop.ini; fl-desktop shows it at once)
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_File_Chooser.H>
#include <algorithm>

#include "fld/ini.h"
#include "fld/pickers.h"
#include "fld/util.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

const char *const kModes[] = {"fill", "fit", "stretch", "center", "tile"};
const char *const kModeLabels[] = {N_("Fill the screen"), N_("Fit (whole picture)"), N_("Stretch"), N_("Center"),
                                   N_("Tile")};

class WallpaperPage : public Fl_Group {
public:
  ConfigFile desk{desktop_ini_path()};
  WallpaperGallery *gallery;
  Fl_Choice *mode;
  std::string wallpaper;

  WallpaperPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Wallpaper"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 700), 0);
    int wx, wy, ww;
    f.row("", 300, wx, wy, ww);
    gallery = new WallpaperGallery(wx, wy, ww, 300);
    gallery->load(WallpaperGallery::default_dirs());
    gallery->callback([](Fl_Widget *, void *d) {
      WallpaperPage *p = (WallpaperPage *)d;
      p->wallpaper = p->gallery->value();
      p->save();
    }, this);
    f.row("", 28, wx, wy, ww);
    Fl_Button *choose = new Fl_Button(wx, wy, 150, 28, _("Other Image…"));
    choose->callback([](Fl_Widget *, void *d) { ((WallpaperPage *)d)->choose(); }, this);
    mode = new Fl_Choice(wx + 230, wy, 190, 28, _("Fit:"));
    for (const char *l : kModeLabels) mode->add(_(l), 0, nullptr, nullptr, 0);
    mode->callback([](Fl_Widget *, void *d) { ((WallpaperPage *)d)->save(); }, this);
    f.space(4);
    f.note(_("Pictures from fldesktop, /usr/share/backgrounds, ~/.local/share/backgrounds and Pictures/Wallpapers; the desktop changes right away."), 2);
    f.space(8);
    reset_button(wx, f.y(), desk, {{"Desktop", "Wallpaper"}, {"Desktop", "WallpaperMode"}}, [this] { load(); });
    g->end();
    end();
    load();
  }

  void load() {
    desk.ini.load(desk.path);
    std::string m = desk.ini.get("Desktop", "WallpaperMode", "fill");
    mode->value(0);
    for (int i = 0; i < 5; i++)
      if (m == kModes[i]) mode->value(i);
    std::string wp = expand_tilde(desk.ini.get("Desktop", "Wallpaper", ""));
    if (wp.empty() || !file_exists(wp)) wp = fld_data_dir() + "/backgrounds/default.svg";
    wallpaper = wp;
    gallery->add_file(wp);
    gallery->value(wp);
  }

  void choose() {
    Fl_File_Chooser fc(dirname(wallpaper).c_str(), "Images (*.{png,jpg,jpeg,svg,gif,bmp,webp,jxl})",
                       Fl_File_Chooser::SINGLE, _("Choose Wallpaper"));
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
    desk.ini.load(desk.path);
    desk.ini.set("Desktop", "Wallpaper", wallpaper);
    desk.ini.set("Desktop", "WallpaperMode", kModes[std::max(0, mode->value())]);
    desk.save();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new WallpaperPage(x, y, w, h); }

} // namespace

PageInfo page_wallpaper() {
  return {"wallpaper", _("Wallpaper"), "preferences-desktop-wallpaper", "Personal",
          _("wallpaper background desktop picture image fill fit stretch center tile"), create};
}

} // namespace fld
