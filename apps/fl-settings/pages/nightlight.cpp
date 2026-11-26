// Night Light: warmer screen colours at night ([NightLight] in desktop.ini,
// applied by fl-nightlight through wlr-gamma-control).
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Value_Slider.H>
#include <algorithm>
#include <cstdio>

#include "fld/ini.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

class NightLightPage : public Fl_Group {
public:
  // night light ([NightLight] in desktop.ini, applied by fl-nightlight)
  Fl_Check_Button *night;
  Fl_Choice *night_schedule;
  Fl_Input *night_from, *night_to;
  Fl_Value_Slider *night_temp;

  ConfigFile desk_file{desktop_ini_path()};

  NightLightPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Night Light"), cy);
    resizable(g);
    Form nf(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    ConfigFile desk(desktop_ini_path());
    const std::string N = "NightLight";
    auto ncb = [](Fl_Widget *, void *d) { ((NightLightPage *)d)->save_night(); };
    nf.row("", 26, wx, wy, ww);
    night = new fld::CheckButton(wx, wy, ww, 26, _("Warmer colours at night"));
    night->value(desk.ini.get_bool(N, "Enabled", false));
    night->callback(ncb, this);
    nf.row(_("When:"), 28, wx, wy, ww);
    night_schedule = new Fl_Choice(wx, wy, 220, 28);
    night_schedule->add(_("Sunset to sunrise|Custom hours|All the time"));
    std::string sch = desk.ini.get(N, "Schedule", "sunset");
    night_schedule->value(sch == "custom" ? 1 : sch == "always" ? 2 : 0);
    night_schedule->callback(ncb, this);
    nf.row(_("From:"), 28, wx, wy, ww);
    night_from = new Fl_Input(wx, wy, 70, 28);
    night_from->value(desk.ini.get(N, "From", "20:00").c_str());
    Fl_Box *to_label = new Fl_Box(wx + 76, wy, 30, 28, _("to"));
    to_label->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER);
    night_to = new Fl_Input(wx + 112, wy, 70, 28);
    night_to->value(desk.ini.get(N, "To", "07:00").c_str());
    for (Fl_Input *in : {night_from, night_to}) {
      in->when(FL_WHEN_RELEASE | FL_WHEN_ENTER_KEY);
      in->callback(ncb, this);
    }
    nf.row(_("Warmth:"), 26, wx, wy, ww);
    night_temp = new Fl_Value_Slider(wx, wy, std::min(ww, 320), 26);
    night_temp->type(FL_HOR_NICE_SLIDER);
    // left: barely warmer, right: very warm; the value shown is in kelvin
    night_temp->bounds(6000, 2500);
    night_temp->step(100);
    night_temp->value(desk.ini.get_int(N, "Temperature", 4000));
    night_temp->when(FL_WHEN_RELEASE);
    night_temp->callback(ncb, this);
    nf.note(_("Sunset and sunrise are worked out from the time zone (Date & Time)."));
    update_night();
    nf.space(8);
    reset_button(nf.widget_x(), nf.y(), desk_file,
                 {{"NightLight", "Enabled"}, {"NightLight", "Schedule"}, {"NightLight", "From"},
                  {"NightLight", "To"}, {"NightLight", "Temperature"}},
                 [this] { load(); });
    g->end();
    end();
  }

  void load() {
    ConfigFile desk(desktop_ini_path());
    const std::string N = "NightLight";
    night->value(desk.ini.get_bool(N, "Enabled", false));
    std::string sch = desk.ini.get(N, "Schedule", "sunset");
    night_schedule->value(sch == "custom" ? 1 : sch == "always" ? 2 : 0);
    night_from->value(desk.ini.get(N, "From", "20:00").c_str());
    night_to->value(desk.ini.get(N, "To", "07:00").c_str());
    night_temp->value(desk.ini.get_int(N, "Temperature", 4000));
    update_night();
  }

  void update_night() {
    bool on = night->value();
    bool custom = night_schedule->value() == 1;
    for (Fl_Widget *w : {(Fl_Widget *)night_schedule, (Fl_Widget *)night_temp})
      on ? w->activate() : w->deactivate();
    for (Fl_Widget *w : {(Fl_Widget *)night_from, (Fl_Widget *)night_to})
      on && custom ? w->activate() : w->deactivate();
  }

  static bool valid_hhmm(const char *s) {
    int h, m;
    char extra;
    return sscanf(s, "%d:%d%c", &h, &m, &extra) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60;
  }

  void save_night() {
    update_night();
    ConfigFile desk(desktop_ini_path());
    const std::string N = "NightLight";
    bool was_on = desk.ini.get_bool(N, "Enabled", false);
    desk.ini.set_bool(N, "Enabled", night->value());
    const char *sch[] = {"sunset", "custom", "always"};
    desk.ini.set(N, "Schedule", sch[std::max(0, night_schedule->value())]);
    if (valid_hhmm(night_from->value())) desk.ini.set(N, "From", night_from->value());
    if (valid_hhmm(night_to->value())) desk.ini.set(N, "To", night_to->value());
    desk.ini.set_int(N, "Temperature", (int)night_temp->value());
    desk.save();
    // fl-nightlight starts with the session; start it now if it is not running
    if (night->value() && !was_on && !which("fl-nightlight").empty())
      ensure_running({"fl-nightlight"});
  }

};

Fl_Group *create(int x, int y, int w, int h) { return new NightLightPage(x, y, w, h); }

} // namespace

PageInfo page_nightlight() {
  return {"nightlight", _("Night Light"), "weather-clear-night", "Hardware",
          _("night light warm colour color temperature blue light evening sunset sunrise redshift"), create};
}

} // namespace fld
