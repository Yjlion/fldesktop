// Notifications: do not disturb, popup timeout, history size, muted applications
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_ask.H>

#include <algorithm>

#include "fld/notify.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

std::string notify_ini_path() { return fld_config_dir() + "/notifications.ini"; }

class NotificationsPage : public Fl_Group {
public:
  ConfigFile cfg{notify_ini_path()};
  Fl_Check_Button *dnd;
  Fl_Spinner *timeout, *history;
  Fl_Hold_Browser *muted;
  std::vector<std::string> muted_apps;

  NotificationsPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Notifications", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    f.heading("Popups");
    f.row("", 26, wx, wy, ww);
    dnd = new Fl_Check_Button(wx, wy, ww, 26, "Do not disturb");
    dnd->value(cfg.ini.get_bool("Notifications", "DoNotDisturb", false));
    dnd->callback([](Fl_Widget *, void *d) { ((NotificationsPage *)d)->save(); }, this);
    f.note("Only critical notifications pop up; the others go to the history.");
    f.row("Show popups for:", 28, wx, wy, ww);
    timeout = new Fl_Spinner(wx, wy, 80, 28);
    timeout->range(1, 120);
    timeout->step(1);
    timeout->value(cfg.ini.get_int("Notifications", "Timeout", 6));
    timeout->callback([](Fl_Widget *, void *d) { ((NotificationsPage *)d)->save(); }, this);
    Fl_Box *sec = new Fl_Box(wx + 86, wy, 120, 28, "seconds");
    sec->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    f.note("Programs may ask for another time. Critical notifications stay until closed.");
    Fl_Button *test = nullptr;
    f.row("", 28, wx, wy, ww);
    test = new Fl_Button(wx, wy, 180, 28, "Send a Test Notification");
    test->callback([](Fl_Widget *, void *) {
      Notification n;
      n.app_name = "Settings";
      n.summary = "Test notification";
      n.body = "Notifications are working.";
      n.icon = "fl-settings";
      n.transient = true;
      notify(n);
    });

    f.heading("History");
    f.row("Keep the last:", 28, wx, wy, ww);
    history = new Fl_Spinner(wx, wy, 80, 28);
    history->range(0, 500);
    history->step(10);
    history->value(cfg.ini.get_int("Notifications", "HistorySize", 100));
    history->callback([](Fl_Widget *, void *d) { ((NotificationsPage *)d)->save(); }, this);
    Fl_Box *nn = new Fl_Box(wx + 86, wy, 160, 28, "notifications");
    nn->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    f.row("", 28, wx, wy, ww);
    Fl_Button *clear = new Fl_Button(wx, wy, 140, 28, "Clear History");
    clear->callback([](Fl_Widget *, void *) { NotifyHistory::get().clear(); });

    f.heading("Muted applications");
    f.note("Notifications of these applications do not pop up (critical ones still do).");
    f.row("", 130, wx, wy, ww);
    muted = new Fl_Hold_Browser(wx, wy, ww - 110, 130);
    muted->textsize(13);
    for (auto &a : split(cfg.ini.get("Notifications", "Muted"), ';'))
      if (!trim(a).empty()) muted_apps.push_back(trim(a));
    Fl_Button *add = new Fl_Button(wx + ww - 100, wy, 100, 28, "Add…");
    add->callback([](Fl_Widget *, void *d) { ((NotificationsPage *)d)->add_app(); }, this);
    Fl_Button *rm = new Fl_Button(wx + ww - 100, wy + 34, 100, 28, "Remove");
    rm->callback([](Fl_Widget *, void *d) {
      NotificationsPage *p = (NotificationsPage *)d;
      int i = p->muted->value() - 1;
      if (i < 0 || i >= (int)p->muted_apps.size()) return;
      p->muted_apps.erase(p->muted_apps.begin() + i);
      p->fill();
      p->save();
    }, this);
    fill();
    g->end();
    end();
  }

  void fill() {
    muted->clear();
    for (auto &a : muted_apps) muted->add(escape_at(a).c_str());
  }

  void add_app() {
    // offer the applications seen in the history
    NotifyHistory::get().fetch([this](const std::vector<HistoryEntry> &list, bool, bool) {
      std::vector<std::string> apps;
      for (auto &e : list)
        if (!e.app.empty() && std::find(apps.begin(), apps.end(), e.app) == apps.end() &&
            std::find(muted_apps.begin(), muted_apps.end(), e.app) == muted_apps.end())
          apps.push_back(e.app);
      std::string hint = apps.empty() ? "" : "Recent: " + join(apps, ", ");
      const char *name = fl_input("%s", apps.empty() ? "" : apps[0].c_str(),
                                  ("Application name as it appears in notifications.\n" + hint).c_str());
      if (!name || !*trim(name).c_str()) return;
      muted_apps.push_back(trim(name));
      fill();
      save();
    });
  }

  void save() {
    cfg.ini.set_bool("Notifications", "DoNotDisturb", dnd->value());
    cfg.ini.set_int("Notifications", "Timeout", (int)timeout->value());
    cfg.ini.set_int("Notifications", "HistorySize", (int)history->value());
    cfg.ini.set("Notifications", "Muted", join(muted_apps, ";"));
    cfg.save(); // fl-notifyd watches the file
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new NotificationsPage(x, y, w, h); }

} // namespace

PageInfo page_notifications() {
  return {"notifications", "Notifications", "preferences-system-notifications", "Personal",
          "notifications popups do not disturb dnd history mute alerts", create};
}

} // namespace fld
