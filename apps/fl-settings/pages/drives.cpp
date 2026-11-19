// Removable Drives: what happens when a drive is plugged in ([Drives] in
// desktop.ini, used by fl-panel)
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

class DrivesPage : public Fl_Group {
public:
  ConfigFile desk{desktop_ini_path()};
  Fl_Check_Button *automount, *autoopen;

  DrivesPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Removable Drives", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    auto cb = [](Fl_Widget *, void *d) { ((DrivesPage *)d)->save(); };
    f.heading("When a Drive Is Plugged In");
    f.row("", 26, wx, wy, ww);
    automount = new Fl_Check_Button(wx, wy, ww, 26, "Mount it");
    automount->value(desk.ini.get_bool("Drives", "Automount", true));
    automount->callback(cb, this);
    f.row("", 26, wx, wy, ww);
    autoopen = new Fl_Check_Button(wx, wy, ww, 26, "Open it in Files");
    autoopen->value(desk.ini.get_bool("Drives", "AutoOpen", false));
    autoopen->callback(cb, this);
    f.note("Otherwise a notification offers to open it. Encrypted drives ask for their passphrase in Files.", 2);
    f.space(12);
    f.note("Drives are listed in the sidebar of Files: click one to mount it, the eject button to remove it safely.", 2);
    g->end();
    end();
    update();
  }
  void update() { automount->value() ? autoopen->activate() : autoopen->deactivate(); }
  void save() {
    update();
    desk.ini.load(desk.path);
    desk.ini.set_bool("Drives", "Automount", automount->value() != 0);
    desk.ini.set_bool("Drives", "AutoOpen", autoopen->value() != 0);
    desk.save();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new DrivesPage(x, y, w, h); }

} // namespace

PageInfo page_drives() {
  return {"drives", "Removable Drives", "drive-removable-media", "Hardware",
          "usb stick drive disk mount automount removable media sd card eject", create};
}

} // namespace fld
