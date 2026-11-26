// Date & Time: time zone and network time through timedatectl (polkit asks for
// permission); without systemd-timedated (Alpine, Devuan) through
// fl-settings-helper run with pkexec
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/fl_ask.H>

#include <functional>
#include <thread>

#include "datetime_sys.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

// runs a command in a thread, reports in the FLTK thread
void run_async(std::vector<std::string> argv, std::function<void(int, const std::string &)> done) {
  struct Res { int code; std::string out; std::function<void(int, const std::string &)> done; };
  std::thread([argv, done] {
    Res *r = new Res{0, "", done};
    r->code = run_capture(argv, r->out);
    Fl::awake([](void *d) {
      Res *r = (Res *)d;
      r->done(r->code, r->out);
      delete r;
    }, r);
  }).detach();
}

class DateTimePage : public Fl_Group {
public:
  bool timedated = !which("timedatectl").empty();
  Fl_Box *now, *zone_label;
  Fl_Check_Button *ntp;
  Fl_Input *filter, *manual;
  Fl_Button *set_manual;
  Fl_Hold_Browser *zones;
  std::vector<std::string> all_zones, shown;
  std::string now_s, zone_s;

  DateTimePage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Date & Time"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    f.row(_("Now:"), 28, wx, wy, ww);
    now = new Fl_Box(wx, wy, ww, 28);
    now->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    now->labelfont(FL_HELVETICA_BOLD);
    f.row("", 26, wx, wy, ww);
    ntp = new fld::CheckButton(wx, wy, ww, 26, _("Set the time automatically (network time)"));
    ntp->callback([](Fl_Widget *, void *d) { ((DateTimePage *)d)->set_ntp(); }, this);
    f.row(_("Set manually:"), 28, wx, wy, ww);
    manual = new Fl_Input(wx, wy, 200, 28);
    manual->tooltip(_("YYYY-MM-DD HH:MM:SS"));
    set_manual = new Fl_Button(wx + 210, wy, 80, 28, _("Set"));
    set_manual->callback([](Fl_Widget *, void *d) { ((DateTimePage *)d)->set_time(); }, this);
    f.heading(_("Time Zone"));
    f.row(_("Current:"), 26, wx, wy, ww);
    zone_label = new Fl_Box(wx, wy, ww, 26);
    zone_label->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    f.row(_("Search:"), 28, wx, wy, ww);
    filter = new Fl_Input(wx, wy, ww, 28);
    filter->when(FL_WHEN_CHANGED);
    filter->callback([](Fl_Widget *, void *d) { ((DateTimePage *)d)->fill_zones(); }, this);
    f.row("", 170, wx, wy, ww);
    zones = new Fl_Hold_Browser(wx, wy, ww, 170);
    zones->textsize(13);
    zones->callback([](Fl_Widget *, void *d) {
      DateTimePage *p = (DateTimePage *)d;
      int i = p->zones->value() - 1;
      if (i < 0 || i >= (int)p->shown.size()) return;
      p->set_zone(p->shown[i]);
    }, this);
    f.note(_("Changing these settings needs administrator rights; you may be asked for a password."), 2);
    g->end();
    end();
    std::string out;
    if (timedated) run_capture({"timedatectl", "list-timezones"}, out);
    all_zones = split(out, '\n');
    if (all_zones.empty()) {
      // no systemd: list /usr/share/zoneinfo regions
      for (auto &r : list_dir("/usr/share/zoneinfo"))
        if (is_dir("/usr/share/zoneinfo/" + r) && isupper((unsigned char)r[0]))
          for (auto &c : list_dir("/usr/share/zoneinfo/" + r)) all_zones.push_back(r + "/" + c);
    }
    fill_zones();
    update();
    Fl::add_timeout(1.0, tick, this);
  }
  ~DateTimePage() override { Fl::remove_timeout(tick, this); }

  static void tick(void *d) {
    DateTimePage *p = (DateTimePage *)d;
    if (p->visible_r()) p->update_clock();
    Fl::repeat_timeout(1.0, tick, d);
  }

  void update_clock() {
    now_s = format_time(time(nullptr), "%A, %d %B %Y   %H:%M:%S");
    now->label(now_s.c_str());
    now->redraw();
  }

  void update() {
    update_clock();
    bool on;
    if (timedated) {
      std::string out;
      run_capture({"timedatectl", "show", "-p", "Timezone", "-p", "NTP", "--value"}, out);
      auto lines = split(out, '\n');
      zone_s = lines.size() > 0 ? lines[0] : getenv_or("TZ", "unknown");
      on = lines.size() > 1 && lines[1] == "yes";
    } else {
      zone_s = dtsys::current_zone();
      if (zone_s.empty()) zone_s = getenv_or("TZ", "unknown");
      on = dtsys::ntp_running();
    }
    zone_label->label(zone_s.c_str());
    ntp->value(on);
    if (on) { manual->deactivate(); set_manual->deactivate(); }
    else { manual->activate(); set_manual->activate(); }
    manual->value(format_time(time(nullptr), "%Y-%m-%d %H:%M:%S").c_str());
  }

  void fill_zones() {
    std::string q = lower(filter->value());
    zones->clear();
    shown.clear();
    for (auto &z : all_zones) {
      if (z.empty() || (!q.empty() && lower(z).find(q) == std::string::npos)) continue;
      shown.push_back(z);
      std::string l = z;
      for (auto &c : l) if (c == '_') c = ' ';
      zones->add(escape_at(l).c_str());
      if (z == zone_s) zones->value(zones->size());
    }
  }

  void result(const char *what, int code, const std::string &out) {
    if (code != 0) fl_alert(_("%s failed%s%s"), what, out.empty() ? "." : ":\n", out.c_str());
    update();
  }

  // timedatectl, else the same change through the root helper
  void change(const char *what, std::vector<std::string> timedatectl, std::vector<std::string> helper) {
    std::vector<std::string> cmd;
    if (timedated) cmd = timedatectl;
    else if (which("pkexec").empty()) {
      fl_alert(_("%s needs timedatectl (systemd) or pkexec (polkit)."), what);
      update();
      return;
    } else {
      cmd = {"pkexec", settings_helper_path(), "system"};
      cmd.insert(cmd.end(), helper.begin(), helper.end());
    }
    std::string w = what;
    run_async(cmd, [this, w](int c, const std::string &o) { result(w.c_str(), c, o); });
  }

  void set_ntp() {
    bool on = ntp->value();
    change("Changing network time", {"timedatectl", "set-ntp", on ? "true" : "false"}, {"ntp", on ? "on" : "off"});
  }
  void set_time() {
    change("Setting the time", {"timedatectl", "set-time", manual->value()}, {"set-time", manual->value()});
  }
  void set_zone(const std::string &z) {
    change("Changing the time zone", {"timedatectl", "set-timezone", z}, {"timezone", z});
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new DateTimePage(x, y, w, h); }

} // namespace

PageInfo page_datetime() {
  return {"datetime", _("Date & Time"), "preferences-system-time", "System",
          _("date time clock timezone time zone ntp network time"), create};
}

} // namespace fld
