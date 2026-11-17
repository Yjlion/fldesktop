// Screen Lock: idle timeout, lock on suspend, what the lock screen shows
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Spinner.H>

#include "fld/pickers.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

class LockPage : public Fl_Group {
public:
  ConfigFile desk{desktop_ini_path()}, wm{wm_ini_path()};
  Fl_Check_Button *auto_lock, *on_suspend, *clock, *clock24, *keyboard, *same_bg;
  Fl_Spinner *minutes;
  WallpaperGallery *gallery;

  LockPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Screen Lock", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    auto cb = [](Fl_Widget *, void *d) { ((LockPage *)d)->save(); };

    f.heading("Locking");
    int idle = wm.ini.get_int("lock", "idle_minutes", 0);
    f.row("", 28, wx, wy, ww);
    auto_lock = new Fl_Check_Button(wx, wy, 230, 28, "Lock the screen after");
    auto_lock->value(idle > 0);
    auto_lock->callback(cb, this);
    minutes = new Fl_Spinner(wx + 236, wy, 70, 28);
    minutes->range(1, 240);
    minutes->value(idle > 0 ? idle : 10);
    minutes->callback(cb, this);
    Fl_Box *mlabel = new Fl_Box(wx + 312, wy, 200, 28, "minutes of inactivity");
    mlabel->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    f.note("Programs playing a video or a presentation can hold the lock off.");
    f.row("", 26, wx, wy, ww);
    on_suspend = new Fl_Check_Button(wx, wy, ww, 26, "Lock the screen before the computer suspends");
    on_suspend->value(desk.ini.get_bool("Lock", "OnSuspend", true));
    on_suspend->callback(cb, this);
    f.row("", 30, wx, wy, ww);
    Fl_Button *now = new Fl_Button(wx, wy, 140, 30, "Lock Now");
    now->callback([](Fl_Widget *, void *) { spawn({"fl-lock"}); });
    Fl_Box *hint = new Fl_Box(wx + 150, wy, ww - 150, 30, "or press Super+L");
    hint->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    hint->labelcolor(palette().fg_dim);

    f.heading("Lock Screen");
    f.row("", 26, wx, wy, ww);
    clock = new Fl_Check_Button(wx, wy, 200, 26, "Show the clock");
    clock->value(desk.ini.get_bool("Lock", "ShowClock", true));
    clock->callback(cb, this);
    clock24 = new Fl_Check_Button(wx + 210, wy, 200, 26, "24-hour clock");
    clock24->value(desk.ini.get_bool("Lock", "Clock24", true));
    clock24->callback(cb, this);
    f.row("", 26, wx, wy, ww);
    keyboard = new Fl_Check_Button(wx, wy, ww, 26, "Offer the on-screen keyboard");
    keyboard->value(desk.ini.get_bool("Lock", "Keyboard", true));
    keyboard->callback(cb, this);
    f.row("Background:", 26, wx, wy, ww);
    std::string bg = expand_tilde(desk.ini.get("Lock", "Background", ""));
    same_bg = new Fl_Check_Button(wx, wy, ww, 26, "The desktop wallpaper");
    same_bg->value(bg.empty());
    same_bg->callback([](Fl_Widget *, void *d) {
      LockPage *p = (LockPage *)d;
      if (p->same_bg->value()) p->gallery->deactivate();
      else p->gallery->activate();
      p->save();
    }, this);
    f.row("", 96, wx, wy, ww);
    gallery = new WallpaperGallery(wx, wy, ww, 96);
    gallery->load(WallpaperGallery::default_dirs());
    if (!bg.empty()) {
      gallery->add_file(bg);
      gallery->value(bg);
    } else {
      gallery->deactivate();
    }
    gallery->callback(cb, this);
    g->end();
    end();
    update();
  }

  void update() {
    if (auto_lock->value()) minutes->activate();
    else minutes->deactivate();
    if (clock->value()) clock24->activate();
    else clock24->deactivate();
  }

  void save() {
    update();
    desk.ini.load(desk.path);
    desk.ini.set_bool("Lock", "OnSuspend", on_suspend->value() != 0);
    desk.ini.set_bool("Lock", "ShowClock", clock->value() != 0);
    desk.ini.set_bool("Lock", "Clock24", clock24->value() != 0);
    desk.ini.set_bool("Lock", "Keyboard", keyboard->value() != 0);
    desk.ini.set("Lock", "Background", same_bg->value() ? "" : gallery->value());
    desk.save();
    wm.ini.load(wm.path);
    wm.ini.set_int("lock", "idle_minutes", auto_lock->value() ? (int)minutes->value() : 0);
    save_wm(wm);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new LockPage(x, y, w, h); }

}  // namespace

PageInfo page_lock() {
  return {"lock", "Screen Lock", "system-lock-screen", "Personal",
          "lock screen locker idle timeout suspend password privacy", create};
}

}  // namespace fld
