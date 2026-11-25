// Privacy: recently used files, and the camera and microphone indicator
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/fl_ask.H>

#include "applets_util.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

// how long recently used files are remembered: GTK's gtk-recent-files-max-age (days, -1 forever)
const int kDays[] = {1, 7, 30, 90, -1};
const char *kDayLabels = "A day|A week|A month|Three months|Until cleared";

void write_gtk(bool enabled, int days) {
  for (const char *v : {"gtk-3.0", "gtk-4.0"}) {
    std::string dir = config_home() + "/" + v, path = dir + "/settings.ini";
    mkdir_p(dir, 0755);
    Ini ini;
    ini.load(path);
    ini.set("Settings", "gtk-recent-files-enabled", enabled ? "true" : "false");
    ini.set_int("Settings", "gtk-recent-files-max-age", enabled ? days : 0);
    ini.save(path);
  }
}

void clear_recent() {
  std::string xbel = data_home() + "/recently-used.xbel";
  if (file_exists(xbel))
    write_file(xbel, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<xbel version=\"1.0\"\n"
                     "      xmlns:bookmark=\"http://www.freedesktop.org/standards/desktop-bookmarks\"\n"
                     "      xmlns:mime=\"http://www.freedesktop.org/standards/shared-mime-info\"\n>\n</xbel>\n");
  // the text editor's own list
  Ini edit;
  std::string ep = fld_config_dir() + "/edit.ini";
  if (edit.load(ep)) {
    edit.set("Editor", "Recent", "");
    edit.save(ep);
  }
}

class PrivacyPage : public Fl_Group {
public:
  ConfigFile desk{desktop_ini_path()}, panel{panel_ini_path()};
  Fl_Check_Button *remember, *indicator;
  Fl_Choice *keep;
  Fl_Box *cam_now;
  std::string cam_text;

  PrivacyPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Privacy", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    auto cb = [](Fl_Widget *, void *d) { ((PrivacyPage *)d)->save(); };

    f.heading(_("Recent Files"));
    f.row("", 26, wx, wy, ww);
    remember = new fld::CheckButton(wx, wy, ww, 26, _("Remember the files I open"));
    remember->value(desk.ini.get_bool("Privacy", "RememberRecent", true));
    remember->callback(cb, this);
    f.row(_("Keep them for:"), 28, wx, wy, ww);
    keep = new Fl_Choice(wx, wy, 200, 28);
    keep->add(kDayLabels);
    int days = desk.ini.get_int("Privacy", "RecentDays", 30);
    keep->value(4);
    for (int i = 0; i < 5; i++)
      if (kDays[i] == days) keep->value(i);
    keep->callback(cb, this);
    f.row("", 30, wx, wy, ww);
    Fl_Button *clear = new Fl_Button(wx, wy, 180, 30, _("Clear Recent Files"));
    clear->callback([](Fl_Widget *, void *) {
      if (fl_choice(_("Forget the list of recently used files?"), _("Cancel"), _("Clear"), nullptr) == 1) clear_recent();
    });
    f.note(_("Programs show the files you opened recently in their menus and file dialogs. The files themselves "
           "are not touched."), 2);

    f.heading(_("Camera and Microphone"));
    f.row("", 26, wx, wy, ww);
    indicator = new fld::CheckButton(wx, wy, ww, 26, _("Show in the panel when the camera or microphone is in use"));
    indicator->value(panel.ini.get_bool("Panel", "PrivacyIndicator", true));
    indicator->callback(cb, this);
    f.row(_("Camera in use by:"), 26, wx, wy, ww);
    cam_now = new Fl_Box(wx, wy, ww, 26);
    cam_now->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    f.note(_("The panel's indicator also names the programs recording from a microphone. Sound > Recording "
           "shows them with their volume."), 2);
    g->end();
    end();
    update();
    poll(this);
  }
  ~PrivacyPage() override { Fl::remove_timeout(poll, this); }

  static void poll(void *d) {
    PrivacyPage *p = (PrivacyPage *)d;
    auto users = applets::camera_users();
    std::string t = users.empty() ? "nothing" : applets::name_list(users, 6);
    if (t != p->cam_text) {
      p->cam_text = t;
      p->cam_now->copy_label(escape_at(t).c_str());
      p->cam_now->labelcolor(users.empty() ? palette().fg_dim : palette().fg);
      p->cam_now->redraw();
    }
    Fl::repeat_timeout(2.0, poll, d);
  }

  void update() {
    if (remember->value()) keep->activate();
    else keep->deactivate();
  }

  void save() {
    update();
    int days = kDays[std::max(0, keep->value())];
    desk.ini.load(desk.path);
    desk.ini.set_bool("Privacy", "RememberRecent", remember->value() != 0);
    desk.ini.set_int("Privacy", "RecentDays", days);
    desk.save();
    write_gtk(remember->value() != 0, days);
    panel.ini.load(panel.path);
    panel.ini.set_bool("Panel", "PrivacyIndicator", indicator->value() != 0);
    panel.save(); // the panel restarts with it
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new PrivacyPage(x, y, w, h); }

}  // namespace

PageInfo page_privacy() {
  return {"privacy", _("Privacy"), "preferences-system-privacy", "Personal",
          _("privacy recent files history recently used camera webcam microphone mic indicator"), create};
}

}  // namespace fld
