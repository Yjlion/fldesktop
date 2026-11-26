// Startup Applications: the XDG autostart entries that fl-autostart starts at
// login (~/.config/autostart over /etc/xdg/autostart).
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <algorithm>

#include "autostart_model.h"
#include "fld/desktop_entry.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

// a label for Fl_Menu_::add(): "/" and "&" taken literally
std::string menu_label(const std::string &s) {
  std::string o;
  for (char c : s) {
    if (c == '/' || c == '\\') o += '\\';
    if (c == '&') o += '&';
    o += c;
  }
  return o;
}

std::string user_dir() { return path_join(config_home(), "autostart"); }

// Asks for a program: one of the applications, or a name and a command.
// Returns false when cancelled.
bool ask_program(std::string &name, std::string &exec, std::string &icon, std::string &comment) {
  static std::vector<DesktopEntry> apps;
  apps = all_applications();
  Fl_Double_Window d(460, 200, _("Add a Startup Program"));
  d.set_modal();
  Fl_Choice app(120, 16, 320, 28, _("Application:"));
  app.add(_("Other command"));
  for (auto &a : apps) app.add(menu_label(a.name).c_str(), 0, nullptr, nullptr, 0);
  app.value(0);
  Fl_Input nm(120, 56, 320, 28, _("Name:"));
  Fl_Input cmd(120, 92, 320, 28, _("Command:"));
  cmd.tooltip(_("The program and its arguments, as in a desktop entry's Exec="));
  struct Ui { Fl_Choice *app; Fl_Input *nm, *cmd; } ui{&app, &nm, &cmd};
  app.callback([](Fl_Widget *, void *p) {
    Ui *u = (Ui *)p;
    int i = u->app->value() - 1;
    if (i >= 0 && i < (int)apps.size()) {
      u->nm->value(apps[i].name.c_str());
      u->cmd->value(apps[i].exec.c_str());
    }
  }, &ui);
  Fl_Button cancel(240, 156, 96, 30, _("Cancel"));
  Fl_Return_Button ok(344, 156, 96, 30, _("Add"));
  static int result;
  result = 0;
  cancel.callback([](Fl_Widget *w, void *) { result = 0; w->window()->hide(); });
  ok.callback([](Fl_Widget *w, void *) { result = 1; w->window()->hide(); });
  d.end();
  d.show();
  while (d.shown()) Fl::wait();
  if (!result) return false;
  name = nm.value();
  exec = cmd.value();
  int i = app.value() - 1;
  if (i >= 0 && i < (int)apps.size() && exec == apps[i].exec) {
    icon = apps[i].icon;
    comment = apps[i].comment;
  }
  return true;
}

class StartupPage : public Fl_Group {
public:
  std::vector<autostart::Entry> entries;
  Fl_Check_Browser *list;

  StartupPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Startup Applications"), cy);
    resizable(g);
    int lw = std::min(W - 48, 640);
    Fl_Box *note = new Fl_Box(X + 24, cy, lw, 22, _("Programs that start when you log in. Unticked ones stay installed but do not start."));
    note->labelsize(12);
    note->labelcolor(palette().fg_dim);
    note->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
    cy += 30;
    list = new Fl_Check_Browser(X + 24, cy, lw, std::max(160, H - (cy - Y) - 70));
    list->callback([](Fl_Widget *, void *d) { ((StartupPage *)d)->toggled(); }, this);
    list->when(FL_WHEN_CHANGED);
    int by = list->y() + list->h() + 10;
    Fl_Button *add = new Fl_Button(X + 24, by, 100, 30, _("Add…"));
    add->callback([](Fl_Widget *, void *d) { ((StartupPage *)d)->add_entry(); }, this);
    Fl_Button *rm = new Fl_Button(X + 24 + 106, by, 100, 30, _("Remove"));
    rm->tooltip(_("Programs installed for everyone are turned off instead"));
    rm->callback([](Fl_Widget *, void *d) { ((StartupPage *)d)->remove_entry(); }, this);
    g->end();
    end();
    fill();
  }

  void fill() {
    entries = autostart::list(config_dirs());
    list->clear();
    for (auto &e : entries) {
      std::string l = e.name;
      if (!e.comment.empty()) l += "  —  " + e.comment;
      list->add(l.c_str(), e.enabled ? 1 : 0);
    }
  }

  void toggled() {
    for (int i = 0; i < (int)entries.size(); i++) {
      bool on = list->checked(i + 1) != 0;
      if (on == entries[i].enabled) continue;
      std::string err;
      if (!autostart::set_enabled(entries[i], on, user_dir(), err)) fl_alert("%s", err.c_str());
      fill();
      return;
    }
  }

  void add_entry() {
    std::string name, exec, icon, comment, err;
    if (!ask_program(name, exec, icon, comment)) return;
    if (autostart::add(user_dir(), name, exec, icon, comment, err).empty()) fl_alert("%s", err.c_str());
    fill();
  }

  void remove_entry() {
    int i = list->value() - 1;
    if (i < 0 || i >= (int)entries.size()) return;
    std::string err;
    if (!autostart::remove(entries[i], user_dir(), err)) fl_alert("%s", err.c_str());
    fill();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new StartupPage(x, y, w, h); }

} // namespace

PageInfo page_startup() {
  return {"startup", _("Startup Applications"), "system-run", "System",
          _("startup applications autostart login start programs session"), create};
}

} // namespace fld
