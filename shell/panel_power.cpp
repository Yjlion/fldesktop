// fl-panel: the battery button from UPower, power profiles, and the low
// battery warnings and action ([Power] in desktop.ini).
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Round_Button.H>

#include <cmath>
#include <string>
#include <vector>

#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/notify.h"
#include "fld/session.h"
#include "fld/atspi.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "panel.h"
#include "upclient.h"
#ifdef FLD_HAVE_BACKLIGHT
#include <FL/Fl_Slider.H>

#include "backlight.h"
#endif

namespace {

fld::FlatButton *g_btn = nullptr;
std::string g_tip;
up::Level g_announced = up::LEVEL_OK;
uint32_t g_note_id = 0;
bool g_action_pending = false;

struct PowerSettings {
  int low = 10, critical = 5, action_level = 3;
  std::string action = "suspend"; // nothing, suspend, hibernate, poweroff
  bool show_percentage = true;
};

PowerSettings settings() {
  fld::Ini c;
  c.load(fld::desktop_config_path());
  PowerSettings s;
  s.low = std::max(1, std::min(50, c.get_int("Power", "LowBattery", s.low)));
  s.critical = std::max(1, std::min(s.low, c.get_int("Power", "CriticalBattery", s.critical)));
  s.action_level = std::max(1, std::min(s.critical, c.get_int("Power", "ActionBattery", s.action_level)));
  s.action = c.get("Power", "CriticalAction", s.action);
  s.show_percentage = c.get_bool("Power", "ShowPercentage", true);
  return s;
}

const char *action_verb(const std::string &a) {
  return a == "hibernate" ? "hibernate" : a == "poweroff" ? "shut down" : "suspend";
}

void run_action(void *) {
  g_action_pending = false;
  const up::State &st = up::Client::get().state();
  PowerSettings ps = settings();
  // plugged in meanwhile?
  if (!st.on_battery || ps.action == "nothing") return;
  fld::run_power(ps.action == "poweroff" ? "poweroff" : ps.action);
}

void warn(up::Level level, const up::State &st) {
  PowerSettings ps = settings();
  fld::Notification n;
  n.app_name = "Power";
  n.category = "device";
  n.replaces_id = g_note_id;
  int pct = (int)std::lround(st.display.percentage);
  std::string left = up::format_time(st.display.time_to_empty);
  if (level == up::LEVEL_LOW) {
    n.summary = "Battery low";
    n.body = std::to_string(pct) + " % left" + (left.empty() ? "" : " (" + left + ")") + ". Plug in the charger soon.";
    n.icon = "battery-caution";
  } else if (level == up::LEVEL_CRITICAL) {
    n.summary = "Battery critically low";
    n.body = std::to_string(pct) + " % left. Plug in the charger now";
    n.body += ps.action == "nothing" ? "." : std::string(", or the computer will ") + action_verb(ps.action) + " at " +
                                                 std::to_string(ps.action_level) + " %.";
    n.icon = "battery-empty";
    n.urgency = 2;
  } else {
    if (ps.action == "nothing") return;
    n.summary = "Battery empty";
    n.body = std::string("The computer will ") + action_verb(ps.action) + " in 30 seconds unless it is plugged in.";
    n.icon = "battery-empty";
    n.urgency = 2;
    if (!g_action_pending) {
      g_action_pending = true;
      Fl::add_timeout(30, run_action);
    }
  }
  fld::notify(n, nullptr, [](uint32_t id) { g_note_id = id; });
}

void update() {
  const up::State &st = up::Client::get().state();
  bool vis = st.running && st.has_battery;
  if (vis) {
    const up::Device &d = st.display;
    panel_icon(g_btn, up::icon_for(d.percentage, d.state, st.on_battery), "battery");
    PowerSettings ps = settings();
    char buf[16];
    snprintf(buf, sizeof buf, "%d%%", (int)std::lround(d.percentage));
    g_btn->copy_label(ps.show_percentage ? buf : nullptr);
    g_tip = up::summary(st);
    if (st.profiles_available && !st.profile.empty()) g_tip += "\nPower mode: " + up::profile_label(st.profile);
    g_btn->tooltip(g_tip.c_str());
    // a screen reader says what the button is, not only "100%"
    fld::atspi_describe(g_btn, 0, "Battery: " + fld::join(fld::split(g_tip, '\n'), ", "));

    // warnings, once per level while discharging
    up::Level lv = up::battery_level(d.percentage, d.state == up::DISCHARGING || st.on_battery, ps.low, ps.critical,
                                     ps.action_level);
    if (lv > g_announced) warn(lv, st);
    if (lv == up::LEVEL_OK && g_announced != up::LEVEL_OK && g_note_id) {
      fld::notify_close(g_note_id);
      g_note_id = 0;
    }
    if (lv < up::LEVEL_ACTION && g_action_pending) { // plugged in: no action
      Fl::remove_timeout(run_action);
      g_action_pending = false;
    }
    g_announced = lv;
  }
  if (vis != (bool)g_btn->visible()) {
    vis ? g_btn->show() : g_btn->hide();
    relayout();
  }
  g_btn->redraw();
}

void battery_cb(Fl_Widget *, void *) {
  if (panel_popup_toggle(g_btn)) return; // open: a second click closes it
  const up::State &st = up::Client::get().state();
  const int W = 300;
  std::vector<const up::Device *> others;
  for (auto &d : st.devices)
    if (!d.power_supply && d.percentage >= 0) others.push_back(&d);
  int bright = -1;
#ifdef FLD_HAVE_BACKLIGHT
  if (!bl::screen_get(bright)) bright = -1;
#endif
  int H = 64 + (bright >= 0 ? 62 : 0) + (st.profiles_available ? 36 + 30 * (int)st.profiles.size() : 0) +
          26 * (int)others.size() + 46;
  PanelPopup pop(W, H);
  static std::string head, sub;
  head = "Battery " + std::to_string((int)std::lround(st.display.percentage)) + "%";
  Fl_Box *title = new Fl_Box(48, 8, W - 58, 24, head.c_str());
  title->labelfont(FL_HELVETICA_BOLD);
  title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  Fl_Box *ic = new Fl_Box(10, 12, 32, 32);
  ic->image(fld::icon_first({up::icon_for(st.display.percentage, st.display.state, st.on_battery), "battery"}, 32));
  sub = fld::escape_at(up::summary(st));
  Fl_Box *s = new Fl_Box(48, 30, W - 58, 22, sub.c_str());
  s->labelsize(12);
  s->labelcolor(fld::palette().fg_dim);
  s->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  int y = 64;
  static std::vector<std::string> keep;
  keep.clear();
  keep.reserve(st.profiles.size() + others.size() + 1);
#ifdef FLD_HAVE_BACKLIGHT
  if (bright >= 0) {
    Fl_Box *bl_label = new Fl_Box(10, y, W - 20, 28, "Brightness");
    bl_label->labelfont(FL_HELVETICA_BOLD);
    bl_label->labelsize(13);
    bl_label->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Slider *sl = new Fl_Slider(16, y + 30, W - 32, 22);
    sl->type(FL_HOR_NICE_SLIDER);
    sl->bounds(1, 100);
    sl->step(1);
    sl->value(bright);
    sl->when(FL_WHEN_CHANGED);
    sl->callback([](Fl_Widget *w, void *) {
      std::string err;
      bl::screen_set((int)((Fl_Slider *)w)->value(), err);
    });
    y += 62;
  }
#endif
  if (st.profiles_available) {
    Fl_Box *pl = new Fl_Box(10, y, W - 20, 28, "Power Mode");
    pl->labelfont(FL_HELVETICA_BOLD);
    pl->labelsize(13);
    pl->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    y += 32;
    for (auto &p : st.profiles) {
      keep.push_back(p);
      Fl_Round_Button *b = new fld::RadioButton(16, y, W - 32, 26);
      b->copy_label(up::profile_label(p).c_str());
      b->type(FL_RADIO_BUTTON);
      b->value(p == st.profile);
      b->callback([](Fl_Widget *w, void *d) {
        up::Client::get().set_profile(*(std::string *)d, [](const std::string &e) {
          if (e.empty()) return;
          fld::Notification n;
          n.summary = "Power mode";
          n.body = e;
          n.transient = true;
          fld::notify(n);
        });
        w->window()->hide();
      }, &keep.back());
      y += 30;
    }
  }
  for (auto *d : others) {
    keep.push_back(d->name() + "  " + std::to_string((int)std::lround(d->percentage)) + "%");
    keep.back() = fld::escape_at(keep.back());
    Fl_Box *b = new Fl_Box(16, y, W - 32, 24, keep.back().c_str());
    b->labelsize(12);
    b->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    y += 26;
  }
  Fl_Button *settings_btn = new Fl_Button(W - 160, H - 38, 150, 28, "Power Settings");
  settings_btn->labelsize(12);
  settings_btn->callback([](Fl_Widget *b, void *) {
    fld::spawn({"fl-settings", "power"});
    b->window()->hide();
  });
  pop.end();
  pop.popup_at(g_btn, true);
}

} // namespace

bool power_indicator_active() {
  const up::State &st = up::Client::get().state();
  return st.running;
}

void power_indicator_start(fld::FlatButton *battery) {
  g_btn = battery;
  g_btn->callback(battery_cb);
  panel_context_menu(g_btn, [](fld::Menu &m) {
    const up::State &st = up::Client::get().state();
    if (st.profiles_available) {
      for (auto &p : st.profiles)
        m.radio(fld::Menu::text(up::profile_label(p)), p == st.profile,
                [p] { up::Client::get().set_profile(p, [](const std::string &) {}); });
      m.divider();
    }
    m.item("Power Settings…", [] { fld::spawn({"fl-settings", "power"}); }).divider();
    panel_menu_items(m);
  });
  up::Client::get().on_changed = update;
  up::Client::get().start();
}
