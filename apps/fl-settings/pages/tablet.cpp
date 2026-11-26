// Tablet & Pen: graphics tablets ([tablet] in wm.ini): which screen the
// tablet covers, and left-handed use. Programs that know tablets (fl-paint
// and others through tablet-v2) get the pen's pressure and tilt.
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <algorithm>

#include "fld/ini.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

// the connected screens by their connector name (HDMI-A-1, eDP-1), as fl-wm names outputs
std::vector<std::string> connected_outputs() {
  std::vector<std::string> out;
  for (auto &n : list_dir("/sys/class/drm")) {
    size_t dash = n.find('-');
    if (dash == std::string::npos || n.compare(0, 4, "card") != 0) continue;
    std::string st;
    if (read_file("/sys/class/drm/" + n + "/status", st) && trim(st) == "connected") out.push_back(n.substr(dash + 1));
  }
  std::sort(out.begin(), out.end());
  return out;
}

class TabletPage : public Fl_Group {
public:
  ConfigFile wm{wm_ini_path()};
  Fl_Choice *output;
  Fl_Check_Button *left;
  std::vector<std::string> outputs;

  TabletPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Tablet & Pen"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    f.row(_("The tablet covers:"), 28, wx, wy, ww);
    output = new Fl_Choice(wx, wy, std::min(ww, 260), 28);
    output->callback([](Fl_Widget *, void *d) { ((TabletPage *)d)->save(); }, this);
    f.row("", 26, wx, wy, ww);
    left = new fld::CheckButton(wx, wy, ww, 26, _("Left-handed (the tablet is turned around)"));
    left->callback([](Fl_Widget *, void *d) { ((TabletPage *)d)->save(); }, this);
    f.note(_("Programs that know tablets get the pen's pressure and tilt; for the others the pen moves the pointer."), 2);
    f.space(8);
    reset_button(f.widget_x(), f.y(), wm, {{"tablet", "output"}, {"tablet", "left_handed"}}, [this] { load(); });
    g->end();
    end();
    load();
  }

  void load() {
    wm.ini.load(wm.path);
    outputs = connected_outputs();
    std::string cur = wm.ini.get("tablet", "output", "");
    if (!cur.empty() && std::find(outputs.begin(), outputs.end(), cur) == outputs.end()) outputs.push_back(cur);
    output->clear();
    output->add(_("All screens"), 0, nullptr, nullptr, 0);
    output->value(0);
    for (size_t i = 0; i < outputs.size(); i++) {
      output->add(outputs[i].c_str(), 0, nullptr, nullptr, 0);
      if (outputs[i] == cur) output->value((int)i + 1);
    }
    left->value(wm.ini.get_bool("tablet", "left_handed", false));
  }

  void save() {
    wm.ini.load(wm.path);
    int i = output->value();
    if (i <= 0 || i > (int)outputs.size()) wm.ini.remove("tablet", "output");
    else wm.ini.set("tablet", "output", outputs[(size_t)i - 1]);
    wm.ini.set_bool("tablet", "left_handed", left->value() != 0);
    save_wm(wm);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new TabletPage(x, y, w, h); }

} // namespace

PageInfo page_tablet() {
  return {"tablet", _("Tablet & Pen"), "input-tablet", "Hardware",
          _("tablet pen stylus wacom drawing graphics tablet left handed screen map pressure"), create};
}

} // namespace fld
