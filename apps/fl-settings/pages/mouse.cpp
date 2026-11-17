// Mouse & Touchpad
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Value_Slider.H>

#include "fld/theme.h"
#include "fld/util.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

class MousePage : public Fl_Group {
public:
  ConfigFile wm{wm_ini_path()};
  Fl_Value_Slider *speed;
  Fl_Check_Button *left, *natural, *tap, *dwt, *middle, *ffm;
  Fl_Choice *scroll;

  Fl_Check_Button *check(Form &f, const char *label, bool v) {
    int wx, wy, ww;
    f.row("", 26, wx, wy, ww);
    Fl_Check_Button *b = new Fl_Check_Button(wx, wy, ww, 26, label);
    b->value(v);
    b->callback([](Fl_Widget *, void *d) { ((MousePage *)d)->save(); }, this);
    return b;
  }

  MousePage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Mouse & Touchpad", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    const std::string P = "pointer";
    f.heading("Pointer");
    f.row("Speed:", 26, wx, wy, ww);
    speed = new Fl_Value_Slider(wx, wy, ww, 26);
    speed->type(FL_HOR_NICE_SLIDER);
    speed->bounds(-1, 1);
    speed->step(0.05);
    speed->value(wm.ini.get_double(P, "accel", 0));
    speed->when(FL_WHEN_RELEASE);
    speed->callback([](Fl_Widget *, void *d) { ((MousePage *)d)->save(); }, this);
    left = check(f, "Left-handed (swap buttons)", wm.ini.get_bool(P, "left_handed", false));
    middle = check(f, "Middle click by pressing left and right together", wm.ini.get_bool(P, "middle_emulation", false));
    f.heading("Scrolling");
    natural = check(f, "Natural scrolling (content follows the fingers)", wm.ini.get_bool(P, "natural_scroll", false));
    f.row("Touchpad scrolling:", 28, wx, wy, ww);
    scroll = new Fl_Choice(wx, wy, 220, 28);
    scroll->add("Device default");
    scroll->add("Two fingers");
    scroll->add("Edge");
    scroll->add("Off");
    std::string sm = wm.ini.get(P, "scroll_method", "");
    scroll->value(sm == "two-finger" ? 1 : sm == "edge" ? 2 : sm == "none" ? 3 : 0);
    scroll->callback([](Fl_Widget *, void *d) { ((MousePage *)d)->save(); }, this);
    f.heading("Touchpad");
    tap = check(f, "Tap to click", wm.ini.get_bool(P, "tap_to_click", true));
    dwt = check(f, "Disable while typing", wm.ini.get_bool(P, "disable_while_typing", true));
    f.heading("Windows");
    ffm = check(f, "Focus follows the mouse", wm.ini.get_bool("behaviour", "focus_follows_mouse", false));
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
    save_wm(wm);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new MousePage(x, y, w, h); }

} // namespace

PageInfo page_mouse() {
  return {"mouse", "Mouse & Touchpad", "input-mouse", "Hardware",
          "mouse touchpad pointer speed acceleration tap click scroll natural left handed focus", create};
}

} // namespace fld
