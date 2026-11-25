// Mouse & Touchpad
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Value_Slider.H>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

class MousePage : public Fl_Group {
public:
  ConfigFile wm{wm_ini_path()};
  Fl_Value_Slider *speed;
  Fl_Check_Button *left, *natural, *tap, *dwt, *middle, *ffm, *gestures, *gnatural;
  Fl_Choice *scroll, *fingers, *gup, *gdown;

  Fl_Check_Button *check(Form &f, const char *label, bool v) {
    int wx, wy, ww;
    f.row("", 26, wx, wy, ww);
    Fl_Check_Button *b = new fld::CheckButton(wx, wy, ww, 26, label);
    b->value(v);
    b->callback([](Fl_Widget *, void *d) { ((MousePage *)d)->save(); }, this);
    return b;
  }

  // actions a vertical swipe can run (value written to wm.ini, label)
  static constexpr const char *kActions[][2] = {
      {"launcher", N_("Open the launcher")}, {"show-desktop", N_("Show the desktop")},
      {"workspace-prev", N_("Previous workspace")}, {"workspace-next", N_("Next workspace")},
      {"none", N_("Nothing")}};

  Fl_Choice *action_choice(Form &f, const char *label, const std::string &cur) {
    int wx, wy, ww;
    f.row(label, 28, wx, wy, ww);
    Fl_Choice *c = new Fl_Choice(wx, wy, 220, 28);
    int sel = 0;
    for (size_t i = 0; i < sizeof kActions / sizeof kActions[0]; i++) {
      c->add(_(kActions[i][1]));
      if (cur == kActions[i][0]) sel = (int)i;
    }
    c->value(sel);
    c->callback([](Fl_Widget *, void *d) { ((MousePage *)d)->save(); }, this);
    return c;
  }

  MousePage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Mouse & Touchpad", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    const std::string P = "pointer";
    f.heading(_("Pointer"));
    f.row(_("Speed:"), 26, wx, wy, ww);
    speed = new Fl_Value_Slider(wx, wy, ww, 26);
    speed->type(FL_HOR_NICE_SLIDER);
    speed->bounds(-1, 1);
    speed->step(0.05);
    speed->value(wm.ini.get_double(P, "accel", 0));
    speed->when(FL_WHEN_RELEASE);
    speed->callback([](Fl_Widget *, void *d) { ((MousePage *)d)->save(); }, this);
    left = check(f, _("Left-handed (swap buttons)"), wm.ini.get_bool(P, "left_handed", false));
    middle = check(f, _("Middle click by pressing left and right together"), wm.ini.get_bool(P, "middle_emulation", false));
    f.heading(_("Scrolling"));
    natural = check(f, _("Natural scrolling (content follows the fingers)"), wm.ini.get_bool(P, "natural_scroll", false));
    f.row(_("Touchpad scrolling:"), 28, wx, wy, ww);
    scroll = new Fl_Choice(wx, wy, 220, 28);
    scroll->add(_("Device default"));
    scroll->add(_("Two fingers"));
    scroll->add(_("Edge"));
    scroll->add(_("Off"));
    std::string sm = wm.ini.get(P, "scroll_method", "");
    scroll->value(sm == "two-finger" ? 1 : sm == "edge" ? 2 : sm == "none" ? 3 : 0);
    scroll->callback([](Fl_Widget *, void *d) { ((MousePage *)d)->save(); }, this);
    f.heading(_("Touchpad"));
    tap = check(f, _("Tap to click"), wm.ini.get_bool(P, "tap_to_click", true));
    dwt = check(f, _("Disable while typing"), wm.ini.get_bool(P, "disable_while_typing", true));
    f.heading(_("Touchpad Gestures"));
    const std::string G = "gestures";
    gestures = check(f, _("Swipe gestures (workspaces and the actions below)"), wm.ini.get_bool(G, "enabled", true));
    f.row(_("Fingers:"), 28, wx, wy, ww);
    fingers = new Fl_Choice(wx, wy, 220, 28);
    fingers->add(_("Three fingers|Four fingers"));
    fingers->value(wm.ini.get_int(G, "fingers", 3) == 4 ? 1 : 0);
    fingers->callback([](Fl_Widget *, void *d) { ((MousePage *)d)->save(); }, this);
    gnatural = check(f, _("Workspaces follow the fingers"), wm.ini.get_bool(G, "natural", true));
    gup = action_choice(f, _("Swipe up:"), wm.ini.get(G, "swipe_up", "launcher"));
    gdown = action_choice(f, _("Swipe down:"), wm.ini.get(G, "swipe_down", "show-desktop"));
    f.note(_("Other gestures, such as pinching to zoom, go to the program under the pointer."), 2);
    f.heading(_("Windows"));
    ffm = check(f, _("Focus follows the mouse"), wm.ini.get_bool("behaviour", "focus_follows_mouse", false));
    g->end();
    end();
  }

  void save() {
    wm.ini.load(wm.path);
    const std::string P = "pointer";
    char buf[32];
    snprintf(buf, sizeof buf, "%.2f", speed->value());
    wm.ini.set(P, "accel", buf);
    wm.ini.set_bool(P, "left_handed", left->value());
    wm.ini.set_bool(P, "middle_emulation", middle->value());
    wm.ini.set_bool(P, "natural_scroll", natural->value());
    const char *sm[] = {"", "two-finger", "edge", "none"};
    if (scroll->value() <= 0) wm.ini.remove(P, "scroll_method");
    else wm.ini.set(P, "scroll_method", sm[scroll->value()]);
    wm.ini.set_bool(P, "tap_to_click", tap->value());
    wm.ini.set_bool(P, "disable_while_typing", dwt->value());
    wm.ini.set_bool("behaviour", "focus_follows_mouse", ffm->value());
    const std::string G = "gestures";
    wm.ini.set_bool(G, "enabled", gestures->value());
    wm.ini.set(G, "fingers", fingers->value() == 1 ? "4" : "3");
    wm.ini.set_bool(G, "natural", gnatural->value());
    wm.ini.set(G, "swipe_up", kActions[gup->value() < 0 ? 0 : gup->value()][0]);
    wm.ini.set(G, "swipe_down", kActions[gdown->value() < 0 ? 1 : gdown->value()][0]);
    save_wm(wm);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new MousePage(x, y, w, h); }

} // namespace

PageInfo page_mouse() {
  return {"mouse", _("Mouse & Touchpad"), "input-mouse", "Hardware",
          _("mouse touchpad pointer speed acceleration tap click scroll natural left handed focus gestures swipe fingers"), create};
}

} // namespace fld
