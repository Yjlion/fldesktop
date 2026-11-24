// Power: battery state (UPower), power mode (power-profiles-daemon), what
// happens when the battery runs low ([Power] in desktop.ini, used by fl-panel)
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <cmath>

#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"
#include "upclient.h"

namespace fld {
namespace {

const char *kActions[] = {"nothing", "suspend", "hibernate", "poweroff"};

class PowerPage : public Fl_Group {
public:
  ConfigFile desk{desktop_ini_path()};
  Fl_Box *bat_icon, *bat_title, *bat_details;
  Fl_Round_Button *profile_btn[3];
  Fl_Box *profile_note;
  Fl_Spinner *low, *critical, *action_at;
  Fl_Choice *action;
  Fl_Check_Button *percentage;
  std::string title_text, details_text, note_text;

  PowerPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Power", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    auto cb = [](Fl_Widget *, void *d) { ((PowerPage *)d)->save(); };

    f.heading("Battery");
    f.row("", 92, wx, wy, ww);
    int fx = X + 24;
    bat_icon = new Fl_Box(fx, wy + 4, 48, 48);
    bat_title = new Fl_Box(fx + 60, wy, ww + (wx - fx) - 60, 26);
    bat_title->labelfont(FL_HELVETICA_BOLD);
    bat_title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    bat_details = new Fl_Box(fx + 60, wy + 26, ww + (wx - fx) - 60, 66);
    bat_details->labelsize(12);
    bat_details->labelcolor(palette().fg_dim);
    bat_details->align(FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    f.row("", 26, wx, wy, ww);
    percentage = new fld::CheckButton(wx, wy, ww, 26, "Show the battery percentage in the panel");
    percentage->value(desk.ini.get_bool("Power", "ShowPercentage", true));
    percentage->callback(cb, this);

    f.heading("Power Mode");
    static const char *labels[] = {"Power Saver — longer battery life", "Balanced", "Performance — faster, more power"};
    static const char *names[] = {"power-saver", "balanced", "performance"};
    for (int i = 0; i < 3; i++) {
      f.row("", 26, wx, wy, ww);
      profile_btn[i] = new fld::RadioButton(wx, wy, ww, 26, labels[i]);
      profile_btn[i]->type(FL_RADIO_BUTTON);
      profile_btn[i]->callback([](Fl_Widget *, void *d) {
        up::Client::get().set_profile((const char *)d, [](const std::string &e) {
          if (!e.empty()) fl_alert("Could not change the power mode:\n%s", escape_at(e).c_str());
        });
      }, (void *)names[i]);
    }
    profile_note = f.note("");

    f.heading("Low Battery");
    f.row("Warn at:", 28, wx, wy, ww);
    low = new Fl_Spinner(wx, wy, 70, 28);
    low->range(2, 50);
    low->value(desk.ini.get_int("Power", "LowBattery", 10));
    low->callback(cb, this);
    unit(wx + 76, wy);
    f.row("Critical at:", 28, wx, wy, ww);
    critical = new Fl_Spinner(wx, wy, 70, 28);
    critical->range(2, 50);
    critical->value(desk.ini.get_int("Power", "CriticalBattery", 5));
    critical->callback(cb, this);
    unit(wx + 76, wy);
    f.row("When the battery is empty:", 28, wx, wy, ww);
    action = new Fl_Choice(wx, wy, 160, 28);
    action->add("Do nothing|Suspend|Hibernate|Shut down");
    std::string a = desk.ini.get("Power", "CriticalAction", "suspend");
    action->value(1);
    for (int i = 0; i < 4; i++)
      if (a == kActions[i]) action->value(i);
    action->callback(cb, this);
    Fl_Box *at = new Fl_Box(wx + 166, wy, 24, 28, "at");
    at->align(FL_ALIGN_CENTER | FL_ALIGN_INSIDE);
    action_at = new Fl_Spinner(wx + 194, wy, 70, 28);
    action_at->range(1, 20);
    action_at->value(desk.ini.get_int("Power", "ActionBattery", 3));
    action_at->callback(cb, this);
    unit(wx + 270, wy);
    f.note("The panel warns once at each level and waits 30 seconds before acting.");

    g->end();
    end();
    up::Client::get().on_changed = [this] { refresh(); };
    up::Client::get().start();
    refresh();
  }

  static void unit(int x, int y) {
    Fl_Box *b = new Fl_Box(x, y, 30, 28, "%");
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  }

  void refresh() {
    const up::State &st = up::Client::get().state();
    if (!st.running) {
      title_text = "Battery information is not available";
      details_text = "UPower is not running (install the upower package).";
      bat_icon->image(icon_first({"battery-missing", "battery"}, 48));
    } else if (!st.has_battery) {
      title_text = st.on_battery ? "On battery" : "No battery";
      details_text = "This computer runs on mains power.";
      bat_icon->image(icon_first({"battery-missing", "battery"}, 48));
    } else {
      const up::Device &d = st.display;
      title_text = escape_at(up::summary(st));
      details_text.clear();
      for (auto &b : st.devices) {
        if (!b.power_supply || b.type != 2) continue;
        std::string line = b.name();
        if (b.capacity > 0) line += " · health " + std::to_string((int)std::lround(b.capacity)) + " %";
        if (b.energy_full > 0) {
          char buf[96];
          snprintf(buf, sizeof buf, " · %.1f of %.1f Wh", b.energy, b.energy_full);
          line += buf;
        }
        if (b.energy_rate > 0.05) {
          char buf[32];
          snprintf(buf, sizeof buf, " · %.1f W", b.energy_rate);
          line += buf;
        }
        if (!b.technology.empty()) line += " · " + b.technology;
        details_text += escape_at(line) + "\n";
      }
      for (auto &b : st.devices) {
        if (b.power_supply || b.percentage < 0) continue;
        details_text += escape_at(b.name() + ": " + std::to_string((int)std::lround(b.percentage)) + " %") + "\n";
      }
      bat_icon->image(icon_first({up::icon_for(d.percentage, d.state, st.on_battery), "battery"}, 48));
    }
    bat_title->label(title_text.c_str());
    bat_details->label(details_text.c_str());
    static const char *names[] = {"power-saver", "balanced", "performance"};
    for (int i = 0; i < 3; i++) {
      bool have = std::find(st.profiles.begin(), st.profiles.end(), names[i]) != st.profiles.end();
      have ? profile_btn[i]->activate() : profile_btn[i]->deactivate();
      profile_btn[i]->value(st.profile == names[i]);
    }
    if (!st.profiles_available) note_text = "Power modes need power-profiles-daemon.";
    else if (!st.degraded.empty()) note_text = "Performance is limited (" + st.degraded + ").";
    else note_text = "";
    profile_note->label(note_text.c_str());
    if (window()) window()->redraw();
  }

  void save() {
    if (critical->value() > low->value()) critical->value(low->value());
    if (action_at->value() > critical->value()) action_at->value(critical->value());
    desk.ini.load(desk.path);
    desk.ini.set_int("Power", "LowBattery", (int)low->value());
    desk.ini.set_int("Power", "CriticalBattery", (int)critical->value());
    desk.ini.set_int("Power", "ActionBattery", (int)action_at->value());
    desk.ini.set("Power", "CriticalAction", kActions[std::max(0, action->value())]);
    desk.ini.set_bool("Power", "ShowPercentage", percentage->value() != 0);
    desk.save();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new PowerPage(x, y, w, h); }

} // namespace

PageInfo page_power() {
  return {"power", "Power", "battery-good", "Hardware",
          "power battery charge suspend hibernate profile performance saver energy laptop", create};
}

} // namespace fld
