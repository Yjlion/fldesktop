// Software Updates: how often fl-packages checks, whether it notifies
#include "fld/i18n.h"
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>

#include <algorithm>

#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

const char *kFreq[][2] = {{"hourly", "Every hour"}, {"daily", "Every day"}, {"weekly", "Every week"},
                          {"never", "Never"}};

class UpdatesPage : public Fl_Group {
public:
  ConfigFile cfg{fld_config_dir() + "/updates.ini"};
  Fl_Choice *freq;
  Fl_Check_Button *notify;

  UpdatesPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Software Updates", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    f.heading(_("Automatic checks"));
    f.row(_("Check for updates:"), 28, wx, wy, ww);
    freq = new Fl_Choice(wx, wy, 200, 28);
    std::string cur = lower(cfg.ini.get("Updates", "Check", "daily"));
    for (int i = 0; i < 4; i++) {
      freq->add(kFreq[i][1]);
      if (cur == kFreq[i][0]) freq->value(i);
    }
    if (freq->value() < 0) freq->value(1);
    freq->callback([](Fl_Widget *, void *d) { ((UpdatesPage *)d)->save(); }, this);
    f.row("", 26, wx, wy, ww);
    notify = new fld::CheckButton(wx, wy, ww, 26, _("Notify when updates are available"));
    notify->value(cfg.ini.get_bool("Updates", "Notify", true));
    notify->callback([](Fl_Widget *, void *d) { ((UpdatesPage *)d)->save(); }, this);
    f.note(_("Updates are never installed without asking. Security updates are marked in the notification."));
    f.heading(_("Software"));
    f.row("", 30, wx, wy, ww);
    Fl_Button *open = new Fl_Button(wx, wy, 200, 30, _("Open Software Packages"));
    open->callback([](Fl_Widget *, void *) { spawn({"fl-packages", "--updates"}); });
    if (which("fl-packages").empty()) {
      open->deactivate();
      f.note(_("fl-packages (PackageKit) is not installed."));
    }
    g->end();
    end();
  }

  void save() {
    int v = std::max(0, freq->value());
    cfg.ini.set("Updates", "Check", kFreq[v][0]);
    cfg.ini.set_bool("Updates", "Notify", notify->value());
    cfg.save();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new UpdatesPage(x, y, w, h); }

} // namespace

PageInfo page_updates() {
  return {"updates", _("Software Updates"), "fl-packages", "System",
          _("updates software packages upgrade security packagekit automatic check"), create};
}

} // namespace fld
