// fl-clock: world clocks, alarms, a timer and a stopwatch.
//
// Alarms are kept in ~/.config/fldesktop/clock.ini. They ring (a notification
// with Snooze and Stop, and a sound) while fl-clock runs, or with
// "fl-clock --alarms": the same without a window, started at login when
// "Ring alarms when Clock is closed" is on.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <signal.h>
#include <unistd.h>

#include <cstdlib>

#include <chrono>
#include <cmath>
#include <string>
#include <vector>

#include "clock_util.h"
#include "fld/config_watch.h"
#include "fld/frame_watch.h"
#include "fld/ini.h"
#include "fld/notify.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

namespace {

std::string ini_path() { return fld::fld_config_dir() + "/clock.ini"; }
std::string autostart_path() { return fld::config_home() + "/autostart/fl-clock-alarms.desktop"; }

double now_mono() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// ---- alarms (shared by the window and --alarms) -----------------------------------------

std::vector<clk::Alarm> g_alarms;
bool g_headless = false;

void load_alarms() {
  std::string text;
  fld::read_file(ini_path(), text);
  g_alarms = clk::alarms_from_ini(text);
}

void save_alarms() {
  // the other settings in the file stay
  fld::Ini ini;
  ini.load(ini_path());
  for (const std::string &g : ini.groups())
    if (fld::starts_with(g, "alarm:")) ini.remove_group(g);
  fld::Ini al;
  al.parse(clk::alarms_to_ini(g_alarms));
  for (const std::string &g : al.groups())
    for (const std::string &k : al.keys(g)) ini.set(g, k, al.get(g, k));
  ini.save(ini_path());
}

void play_sound() {
  for (const char *f : {"/usr/share/sounds/freedesktop/stereo/alarm-clock-elapsed.oga",
                        "/usr/share/sounds/freedesktop/stereo/complete.oga"}) {
    if (!fld::file_exists(f)) continue;
    if (!fld::which("pw-play").empty()) fld::spawn({"pw-play", f});
    else if (!fld::which("paplay").empty()) fld::spawn({"paplay", f});
    else if (!fld::which("canberra-gtk-play").empty()) fld::spawn({"canberra-gtk-play", "-i", "alarm-clock-elapsed"});
    return;
  }
}

void ring(const clk::Alarm &a) {
  fld::Notification n;
  n.app_name = "Clock";
  n.summary = a.label.empty() ? "Alarm" : a.label;
  char t[16];
  snprintf(t, sizeof t, "%02d:%02d", a.hour, a.minute);
  n.body = std::string("It is ") + t + ".";
  n.icon = "alarm";
  n.urgency = 2;
  n.timeout = 0;
  n.actions = {{"snooze", "Snooze 10 min"}, {"stop", "Stop"}};
  std::string id = a.id;
  fld::notify(n, [id](const std::string &action) {
    if (action != "snooze") return;
    load_alarms();
    for (clk::Alarm &x : g_alarms)
      if (x.id == id) x.snoozed_until = time(nullptr) + 600;
    save_alarms();
  });
  play_sound();
}

// once a few seconds: alarms whose time has come
void check_alarms(void *) {
  time_t now = time(nullptr);
  bool changed = false;
  for (clk::Alarm &a : g_alarms) {
    if (!a.enabled) continue;
    // the minute it was due (not rung for that minute yet)
    time_t due = clk::next_ring(a, now - 61);
    if (due && due <= now && a.last_rang < due) {
      a.last_rang = due;
      if (a.snoozed_until && a.snoozed_until <= now) a.snoozed_until = 0;
      if (a.days == 0 && !a.snoozed_until) a.enabled = false; // once
      ring(a);
      changed = true;
    }
  }
  if (changed) save_alarms();
  Fl::repeat_timeout(5, check_alarms);
}

void set_background(bool on) {
  fld::Ini ini;
  ini.load(ini_path());
  ini.set_bool("Clock", "RingWhenClosed", on);
  ini.save(ini_path());
  if (on) {
    fld::mkdir_p(fld::dirname(autostart_path()));
    fld::write_file(autostart_path(), "[Desktop Entry]\nType=Application\nName=Clock alarms\n"
                                      "Exec=fl-clock --alarms\nNoDisplay=true\n");
  } else {
    unlink(autostart_path().c_str());
  }
}

// ---- the window ---------------------------------------------------------------------------

Fl_Double_Window *g_win;
Fl_Tabs *g_tabs;
// starts or stops the page timers after a change (clock running, page shown)
void reschedule();

// world clocks
std::vector<std::string> g_zones; // "" = here
class ClockFace;
class WorldPage : public Fl_Group {
public:
  Fl_Hold_Browser *list;
  int cols[5] = {260, 140, 160, 0, 0};
  WorldPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H, "World Clocks") {
    list = new Fl_Hold_Browser(X + 10, Y + 10, W - 20, H - 60);
    list->textsize(16);
    list->has_scrollbar(Fl_Browser_::VERTICAL);
    list->column_char('\t');
    list->column_widths(cols);
    fit_columns();
    Fl_Button *add = new Fl_Button(X + 10, Y + H - 42, 120, 30, "Add City…");
    add->callback([](Fl_Widget *, void *d) { ((WorldPage *)d)->add_zone(); }, this);
    Fl_Button *rm = new Fl_Button(X + 136, Y + H - 42, 100, 30, "Remove");
    rm->callback([](Fl_Widget *, void *d) { ((WorldPage *)d)->remove_zone(); }, this);
    end();
    resizable(list);
  }
  // the columns share the list's width: city, time, day, the offset last
  void fit_columns() {
    int w = list->w() - Fl::scrollbar_size() - 8;
    cols[0] = w * 36 / 100;
    cols[1] = w * 19 / 100;
    cols[2] = w * 22 / 100;
    cols[3] = 0;
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Group::resize(X, Y, W, H);
    fit_columns();
    list->redraw();
  }
  void refresh() {
    time_t t = time(nullptr);
    bool rebuild = list->size() != (int)g_zones.size();
    int sel = list->value(), top = list->topline();
    if (rebuild) list->clear();
    for (size_t i = 0; i < g_zones.size(); i++) {
      const std::string &z = g_zones[i];
      struct tm tm = clk::time_in(z, t);
      char hm[16], day[32];
      strftime(hm, sizeof hm, "%H:%M:%S", &tm);
      strftime(day, sizeof day, "%a %d %b", &tm);
      std::string name = z.empty() ? "Here" : clk::city_of(z);
      std::string off = z.empty() ? "" : clk::offset_text(z, t);
      std::string line = "@b" + fld::escape_at(name) + "\t@b" + hm + "\t" + day + "\t" + off;
      // rows are changed in place: rebuilding the list every second would
      // lose the selection and the scroll position for a moment
      if (rebuild) list->add(line.c_str());
      else if (line != list->text((int)i + 1)) list->text((int)i + 1, line.c_str());
    }
    if (rebuild) {
      list->topline(top);
      if (sel) list->select(sel);
    }
  }
  void save() {
    fld::Ini ini;
    ini.load(ini_path());
    std::vector<std::string> z(g_zones.begin() + 1, g_zones.end());
    ini.set("Clock", "Zones", fld::join(z, ";"));
    ini.save(ini_path());
  }
  void add_zone() {
    std::string tab;
    if (!fld::read_file("/usr/share/zoneinfo/zone1970.tab", tab)) fld::read_file("/usr/share/zoneinfo/zone.tab", tab);
    std::vector<std::string> zones = clk::parse_zone_tab(tab);
    if (zones.empty()) {
      fl_alert("No time zone list (tzdata is not installed).");
      return;
    }
    // a searchable list
    Fl_Double_Window d(420, 480, "Add City");
    d.set_modal();
    Fl_Input find(70, 10, 340, 28, "Find:");
    find.when(FL_WHEN_CHANGED);
    Fl_Hold_Browser l(10, 46, 400, 384);
    struct Ctx {
      std::vector<std::string> *all;
      std::vector<std::string> shown;
      Fl_Hold_Browser *list;
      Fl_Input *find;
    } ctx{&zones, {}, &l, &find};
    auto fill = [](Ctx *c) {
      std::string q = fld::lower(c->find->value());
      c->list->clear();
      c->shown.clear();
      for (const std::string &z : *c->all)
        if (q.empty() || fld::lower(z).find(q) != std::string::npos) {
          c->shown.push_back(z);
          c->list->add((clk::city_of(z) + "  (" + z + ")").c_str());
        }
    };
    fill(&ctx);
    find.callback([](Fl_Widget *, void *v) {
      Ctx *c = (Ctx *)v;
      std::string q = fld::lower(c->find->value());
      c->list->clear();
      c->shown.clear();
      for (const std::string &z : *c->all)
        if (q.empty() || fld::lower(z).find(q) != std::string::npos) {
          c->shown.push_back(z);
          c->list->add((clk::city_of(z) + "  (" + z + ")").c_str());
        }
    }, &ctx);
    Fl_Button cancel(196, 440, 100, 30, "Cancel");
    Fl_Return_Button ok(304, 440, 106, 30, "Add");
    int result = 0;
    cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 2; }, &result);
    ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
    l.callback([](Fl_Widget *, void *r) {
      if (Fl::event_clicks()) *(int *)r = 1;
    }, &result);
    d.end();
    d.show();
    while (!result && d.shown()) Fl::wait();
    if (result != 1 || l.value() <= 0) return;
    g_zones.push_back(ctx.shown[(size_t)l.value() - 1]);
    save();
    refresh();
  }
  void remove_zone() {
    int i = list->value() - 1;
    if (i <= 0 || i >= (int)g_zones.size()) return; // "Here" stays
    g_zones.erase(g_zones.begin() + i);
    save();
    refresh();
  }
};

// alarms
class AlarmsPage : public Fl_Group {
public:
  Fl_Hold_Browser *list;
  Fl_Check_Button *bg;
  AlarmsPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H, "Alarms") {
    list = new Fl_Hold_Browser(X + 10, Y + 10, W - 20, H - 96);
    list->textsize(16);
    static int cols[] = {120, 220, 0};
    list->column_widths(cols);
    list->column_char('\t');
    list->callback([](Fl_Widget *, void *d) {
      if (Fl::event_clicks() && Fl::event() == FL_RELEASE) ((AlarmsPage *)d)->edit(false);
    }, this);
    int x = X + 10;
    auto btn = [&](const char *l, int w, Fl_Callback *cb) {
      Fl_Button *b = new Fl_Button(x, Y + H - 78, w, 30, l);
      b->callback(cb, this);
      x += w + 6;
    };
    btn("New Alarm…", 110, [](Fl_Widget *, void *d) { ((AlarmsPage *)d)->edit(true); });
    btn("Edit…", 80, [](Fl_Widget *, void *d) { ((AlarmsPage *)d)->edit(false); });
    btn("On / Off", 80, [](Fl_Widget *, void *d) { ((AlarmsPage *)d)->toggle(); });
    btn("Delete", 80, [](Fl_Widget *, void *d) { ((AlarmsPage *)d)->remove(); });
    bg = new fld::CheckButton(X + 10, Y + H - 40, W - 20, 28, "Ring alarms when Clock is closed (starts with the session)");
    fld::Ini ini;
    ini.load(ini_path());
    bg->value(ini.get_bool("Clock", "RingWhenClosed", false));
    bg->callback([](Fl_Widget *w, void *) {
      bool on = ((Fl_Check_Button *)w)->value();
      set_background(on);
      if (on) fld::spawn({"fl-clock", "--alarms"});
    });
    end();
    resizable(list);
  }
  void refresh() {
    int sel = list->value();
    list->clear();
    time_t now = time(nullptr);
    for (const clk::Alarm &a : g_alarms) {
      char t[16];
      snprintf(t, sizeof t, "%02d:%02d", a.hour, a.minute);
      std::string next;
      time_t n = clk::next_ring(a, now);
      if (n) {
        double h = (n - now) / 3600.0;
        char b[64];
        if (h < 24) snprintf(b, sizeof b, "in %d h %02d min", (int)h, (int)((n - now) / 60 % 60));
        else snprintf(b, sizeof b, "in %d days", (int)(h / 24));
        next = b;
      }
      std::string line = std::string(a.enabled ? "@b" : "@i@C8") + t + "\t" + (a.enabled ? "" : "@i@C8") +
                         clk::days_text(a.days) + "\t" + (a.enabled ? "" : "@i@C8") + a.label +
                         (next.empty() ? "" : "   (" + next + ")");
      list->add(line.c_str());
    }
    if (g_alarms.empty()) list->add("@iNo alarms. Make one with New Alarm.");
    if (sel) list->select(std::min(sel, list->size()));
  }
  clk::Alarm *selected() {
    int i = list->value() - 1;
    return i >= 0 && i < (int)g_alarms.size() ? &g_alarms[(size_t)i] : nullptr;
  }
  void edit(bool is_new) {
    clk::Alarm a;
    if (!is_new) {
      if (!selected()) return;
      a = *selected();
    } else {
      a.id = std::to_string((long long)time(nullptr)) + "-" + std::to_string(rand() % 1000);
      a.days = 0x3e;
    }
    Fl_Double_Window d(400, 250, is_new ? "New Alarm" : "Edit Alarm");
    d.set_modal();
    Fl_Spinner hh(90, 16, 60, 32, "Time:");
    hh.range(0, 23);
    hh.value(a.hour);
    Fl_Spinner mm(156, 16, 60, 32);
    mm.range(0, 59);
    mm.value(a.minute);
    Fl_Input label(90, 58, 290, 28, "Label:");
    label.value(a.label.c_str());
    static const char *dn[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    static const int bit[] = {1, 2, 3, 4, 5, 6, 0};
    Fl_Check_Button *days[7];
    Fl_Box dl(10, 96, 76, 26, "Repeat:");
    dl.align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
    for (int i = 0; i < 7; i++) {
      days[i] = new fld::CheckButton(90 + (i % 4) * 72, 96 + (i / 4) * 28, 70, 26, dn[i]);
      days[i]->value((a.days >> bit[i]) & 1);
    }
    Fl_Box note(90, 154, 300, 40, "No days ticked: it rings once.");
    note.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP);
    note.labelsize(12);
    note.labelcolor(fld::palette().fg_dim);
    Fl_Button cancel(170, 204, 100, 30, "Cancel");
    Fl_Return_Button ok(278, 204, 106, 30, "Save");
    int result = 0;
    cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 2; }, &result);
    ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
    d.end();
    d.show();
    while (!result && d.shown()) Fl::wait();
    if (result != 1) return;
    a.hour = (int)hh.value();
    a.minute = (int)mm.value();
    a.label = label.value();
    a.days = 0;
    for (int i = 0; i < 7; i++)
      if (days[i]->value()) a.days |= 1 << bit[i];
    a.enabled = true;
    a.snoozed_until = 0;
    a.last_rang = 0;
    load_alarms();
    bool found = false;
    for (clk::Alarm &x : g_alarms)
      if (x.id == a.id) x = a, found = true;
    if (!found) g_alarms.push_back(a);
    save_alarms();
    load_alarms();
    refresh();
  }
  void toggle() {
    clk::Alarm *a = selected();
    if (!a) return;
    a->enabled = !a->enabled;
    a->snoozed_until = 0;
    save_alarms();
    refresh();
  }
  void remove() {
    clk::Alarm *a = selected();
    if (!a) return;
    g_alarms.erase(g_alarms.begin() + (a - g_alarms.data()));
    save_alarms();
    refresh();
  }
};

// a big centred time
class BigTime : public Fl_Box {
public:
  using Fl_Box::Fl_Box;
  std::string text;
  void draw() override {
    fl_color(fld::palette().bg2);
    fl_rectf(x(), y(), w(), h());
    fl_color(fld::palette().fg);
    fl_font(FL_HELVETICA, std::min(h() * 2 / 3, w() / 6));
    fl_draw(text.c_str(), x(), y(), w(), h(), FL_ALIGN_CENTER);
  }
  void set(const std::string &t) {
    if (t == text) return;
    text = t;
    redraw();
  }
};

// timer
class TimerPage : public Fl_Group {
public:
  BigTime *show;
  Fl_Input *in;
  Fl_Button *start, *reset;
  double total = 300, left = 300, end_at = 0; // end_at: monotonic, 0 = not running
  TimerPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H, "Timer") {
    show = new BigTime(X + 10, Y + 10, W - 20, H - 110);
    in = new Fl_Input(X + 120, Y + H - 92, 140, 30, "Duration:");
    in->value("5:00");
    in->tooltip("Minutes (5), minutes:seconds (1:30), h:mm:ss or 1h30m");
    int x = X + 10;
    for (int m : {1, 5, 10, 25, 60}) {
      Fl_Button *b = new Fl_Button(x + 270, Y + H - 92, 54, 30);
      b->copy_label((std::to_string(m) + " min").c_str());
      b->labelsize(12);
      b->callback([](Fl_Widget *w, void *d) {
        TimerPage *p = (TimerPage *)d;
        p->in->value((std::string(w->label()).substr(0, std::string(w->label()).find(' ')) + ":00").c_str());
        p->set_from_input();
      }, this);
      x += 58;
    }
    start = new Fl_Button(X + 10, Y + H - 50, 120, 36, "Start");
    start->callback([](Fl_Widget *, void *d) { ((TimerPage *)d)->toggle(); }, this);
    reset = new Fl_Button(X + 136, Y + H - 50, 100, 36, "Reset");
    reset->callback([](Fl_Widget *, void *d) { ((TimerPage *)d)->set_from_input(); }, this);
    end();
    resizable(show);
    tick();
  }
  void set_from_input() {
    double d = clk::parse_duration(in->value());
    if (d <= 0) {
      fl_beep();
      return;
    }
    total = left = d;
    end_at = 0;
    start->label("Start");
    tick();
    reschedule();
  }
  void toggle() {
    if (end_at) {
      left = std::max(0.0, end_at - now_mono());
      end_at = 0;
      start->label("Go On");
    } else {
      if (left <= 0) left = total;
      end_at = now_mono() + left;
      start->label("Pause");
    }
    reschedule();
  }
  void tick() {
    double l = end_at ? std::max(0.0, end_at - now_mono()) : left;
    show->set(clk::format_duration(std::ceil(l)));
    if (end_at && l <= 0) {
      end_at = 0;
      left = 0;
      start->label("Start");
      fld::Notification n;
      n.app_name = "Clock";
      n.summary = "Time is up";
      n.body = clk::format_duration(total) + " have passed.";
      n.icon = "alarm";
      n.urgency = 2;
      fld::notify(n);
      play_sound();
    }
  }
};

// stopwatch
class StopwatchPage : public Fl_Group {
public:
  BigTime *show;
  Fl_Hold_Browser *laps;
  Fl_Button *start, *lap;
  double base = 0, started = 0; // base: time before the last start
  StopwatchPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H, "Stopwatch") {
    show = new BigTime(X + 10, Y + 10, W - 20, (H - 60) / 2);
    laps = new Fl_Hold_Browser(X + 10, Y + 14 + (H - 60) / 2, W - 20, (H - 60) / 2 - 4);
    laps->textsize(14);
    start = new Fl_Button(X + 10, Y + H - 44, 120, 34, "Start");
    start->callback([](Fl_Widget *, void *d) { ((StopwatchPage *)d)->toggle(); }, this);
    lap = new Fl_Button(X + 136, Y + H - 44, 100, 34, "Lap");
    lap->callback([](Fl_Widget *, void *d) { ((StopwatchPage *)d)->add_lap(); }, this);
    Fl_Button *reset = new Fl_Button(X + 242, Y + H - 44, 100, 34, "Reset");
    reset->callback([](Fl_Widget *, void *d) {
      StopwatchPage *p = (StopwatchPage *)d;
      p->base = 0;
      p->started = p->started ? now_mono() : 0;
      p->laps->clear();
      p->tick();
    }, this);
    end();
    resizable(laps);
    tick();
  }
  double elapsed() const { return base + (started ? now_mono() - started : 0); }
  void toggle() {
    if (started) {
      base = elapsed();
      started = 0;
      start->label("Go On");
    } else {
      started = now_mono();
      start->label("Stop");
    }
    reschedule();
  }
  void add_lap() {
    laps->insert(1, ("Lap " + std::to_string(laps->size() + 1) + "    " + clk::format_duration(elapsed(), true)).c_str());
  }
  void tick() { show->set(clk::format_duration(elapsed(), true)); }
};

WorldPage *g_world;
AlarmsPage *g_alarms_page;
TimerPage *g_timer;
StopwatchPage *g_stopwatch;

// The pages redraw only what is shown: the world clocks and a running timer
// once a second, on the second; a running stopwatch ten times a second. Nothing
// while the window is minimised or on another workspace.
fld::FrameWatch *g_watch;

double wall_now() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

void fast_tick(void *);
void second_tick(void *);

bool shown(Fl_Widget *page) { return g_tabs->value() == page && g_watch->visible(); }

void second_tick(void *) {
  bool on_screen = g_watch->visible();
  if (on_screen && g_tabs->value() == g_world) g_world->refresh();
  if (on_screen && g_stopwatch->started && g_tabs->value() == g_stopwatch && !Fl::has_timeout(fast_tick))
    Fl::add_timeout(0.1, fast_tick);
  // a running timer keeps going (it rings when it ends) even when not shown
  if (g_timer->end_at || (on_screen && g_tabs->value() == g_timer)) g_timer->tick();
  double delay = 1.0 - std::fmod(wall_now(), 1.0) + 0.002;
  if (g_timer->end_at) {
    // the timer shows whole seconds left: change it as one passes
    double left = g_timer->end_at - now_mono();
    double frac = left - std::floor(left);
    if (frac > 0.001 && frac < delay) delay = frac + 0.002;
  }
  Fl::repeat_timeout(delay, second_tick);
}

void fast_tick(void *) {
  if (!g_stopwatch->started || !shown(g_stopwatch)) return; // second_tick checks back
  g_stopwatch->tick();
  Fl::repeat_timeout(0.1, fast_tick);
}

void reschedule() {
  if (!g_tabs || !g_watch) return;
  Fl::remove_timeout(second_tick);
  Fl::remove_timeout(fast_tick);
  g_world->refresh();
  g_timer->tick();
  g_stopwatch->tick();
  Fl::add_timeout(1.0 - std::fmod(wall_now(), 1.0) + 0.002, second_tick);
  if (g_stopwatch->started && g_tabs->value() == g_stopwatch) Fl::add_timeout(0.1, fast_tick);
}

} // namespace

int main(int argc, char **argv) {
  bool alarms_only = false;
  std::string page;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--alarms") alarms_only = true;
    else if (a == "--page" && i + 1 < argc) page = argv[++i];
    else if (a == "-h" || a == "--help") {
      printf("Usage: fl-clock [--page world|alarms|timer|stopwatch]\n"
             "       fl-clock --alarms   ring alarms without a window\n");
      return 0;
    }
  }
  fld::init_app("fl-clock", argc, argv);
  Fl::lock();
  load_alarms();
  // alarms change in the file (the window, or another fl-clock)
  static fld::ConfigWatcher watch(ini_path(), [] {
    load_alarms();
    if (g_alarms_page) g_alarms_page->refresh();
  });
  if (alarms_only) {
    // one ringer per session: a lock file in the runtime directory
    std::string lock = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-clock-alarms.pid";
    std::string pid;
    if (fld::read_file(lock, pid) && atoi(pid.c_str()) > 1 && kill(atoi(pid.c_str()), 0) == 0) return 0;
    fld::write_file(lock, std::to_string(getpid()));
    g_headless = true;
    Fl::add_timeout(1, check_alarms);
    while (true) Fl::wait(1e20);
  }
  fld::Ini ini;
  ini.load(ini_path());
  g_zones.push_back("");
  for (const std::string &z : fld::split(ini.get("Clock", "Zones", "Europe/London;America/New_York;Asia/Tokyo"), ';'))
    if (!fld::trim(z).empty()) g_zones.push_back(fld::trim(z));

  const int W = 640, H = 480;
  g_win = new Fl_Double_Window(W, H, "Clock");
  g_tabs = new fld::Tabs(0, 0, W, H);
  g_world = new WorldPage(0, 30, W, H - 30);
  g_alarms_page = new AlarmsPage(0, 30, W, H - 30);
  g_alarms_page->hide();
  g_timer = new TimerPage(0, 30, W, H - 30);
  g_timer->hide();
  g_stopwatch = new StopwatchPage(0, 30, W, H - 30);
  g_stopwatch->hide();
  g_tabs->end();
  g_win->end();
  g_win->resizable(g_tabs);
  g_win->size_range(420, 340);
  fld::set_window_icon(g_win, "fl-clock");
  if (page == "alarms") g_tabs->value(g_alarms_page);
  else if (page == "timer") g_tabs->value(g_timer);
  else if (page == "stopwatch") g_tabs->value(g_stopwatch);
  g_world->refresh();
  g_alarms_page->refresh();
  g_win->show();
  g_watch = new fld::FrameWatch(g_win);
  g_watch->on_shown([](void *) { reschedule(); }, nullptr);
  g_tabs->when(FL_WHEN_CHANGED);
  g_tabs->callback([](Fl_Widget *, void *) { reschedule(); });
  reschedule();
  // the window rings alarms unless the background ringer runs
  if (!ini.get_bool("Clock", "RingWhenClosed", false)) Fl::add_timeout(1, check_alarms);
  return Fl::run();
}
