// Windows & Workspaces: how fl-wm handles windows ([behaviour] and
// [appearance] in wm.ini): focus, workspaces, snapping, moving, hot corners,
// title bars and shadows.
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Spinner.H>
#include <algorithm>

#include "fld/ini.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

const char *const B = "behaviour";
const char *const A = "appearance";

// hot corners: wm.ini keys and the actions offered (value, label)
const char *const kCornerKeys[] = {"hot_corner_top_left", "hot_corner_top_right", "hot_corner_bottom_left",
                                   "hot_corner_bottom_right"};
const char *const kCornerLabels[] = {N_("Top left corner:"), N_("Top right corner:"), N_("Bottom left corner:"),
                                     N_("Bottom right corner:")};
const char *const kCornerActions[][2] = {
    {"", N_("Nothing")},        {"overview", N_("Overview of the windows")},
    {"launcher", N_("Applications")}, {"show-desktop", N_("Show the desktop")},
    {"lock", N_("Lock the screen")},  {"workspace-next", N_("Next workspace")},
};
const int kNCornerActions = sizeof kCornerActions / sizeof kCornerActions[0];

// drag modifier: wm.ini values, in the order of the choice
const char *const kDragMods[] = {"super", "alt", "super+alt", "none"};

class WindowsPage : public Fl_Group {
public:
  ConfigFile wm{wm_ini_path()};
  Fl_Check_Button *ffm, *switcher, *drag_snap, *assist, *shadows, *accent_border, *restore, *tiling, *per_output;
  Fl_Spinner *workspaces, *title_height;
  Fl_Choice *drag_mod, *corner[4];

  WindowsPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Windows & Workspaces"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    auto cb = [](Fl_Widget *, void *d) { ((WindowsPage *)d)->save(); };

    f.heading(_("Focus"));
    ffm = check(f, _("Focus follows the mouse"));
    switcher = check(f, _("Alt+Tab shows a list of the windows"));
    switcher->tooltip(_("Off: each Alt+Tab raises the next window directly"));

    f.heading(_("Workspaces"));
    f.row(_("Number of workspaces:"), 28, wx, wy, ww);
    workspaces = new Fl_Spinner(wx, wy, 70, 28);
    workspaces->type(FL_INT_INPUT);
    workspaces->range(1, 9);
    workspaces->step(1);
    workspaces->when(FL_WHEN_CHANGED);
    workspaces->callback(cb, this);
    per_output = check(f, _("Each screen switches its own workspace"));
    per_output->tooltip(_("Off: all screens switch workspaces together"));
    restore = check(f, _("Reopen the windows of the last session at login"));
    restore->tooltip(_("The programs start again and their windows go back to their workspace, place and size"));

    f.heading(_("Snapping and Moving"));
    tiling = check(f, _("Tile the windows (the oldest on the left, the others beside it)"));
    tiling->tooltip(_("Super+Shift+T tiles or stacks the windows of one workspace"));
    drag_snap = check(f, _("Snap windows dragged to an edge or a corner of the screen"));
    assist = check(f, _("Snap assist: after snapping a window to a side, offer the others for the rest"));
    f.row(_("Move windows with:"), 28, wx, wy, ww);
    drag_mod = new Fl_Choice(wx, wy, 220, 28);
    drag_mod->add(_("Super+drag|Alt+drag|Super or Alt+drag|Only the title bar"));
    drag_mod->tooltip(_("Hold this key and drag with the left button to move a window, with the right button to resize it"));
    drag_mod->callback(cb, this);

    f.heading(_("Hot Corners"));
    for (int i = 0; i < 4; i++) {
      f.row(_(kCornerLabels[i]), 28, wx, wy, ww);
      corner[i] = new Fl_Choice(wx, wy, 240, 28);
      for (int k = 0; k < kNCornerActions; k++) corner[i]->add(_(kCornerActions[k][1]));
      corner[i]->callback(cb, this);
    }

    f.heading(_("Title Bars and Shadows"));
    f.row(_("Title bar height:"), 28, wx, wy, ww);
    title_height = new Fl_Spinner(wx, wy, 70, 28);
    title_height->type(FL_INT_INPUT);
    title_height->range(18, 60);
    title_height->step(1);
    title_height->when(FL_WHEN_RELEASE | FL_WHEN_ENTER_KEY);
    title_height->callback(cb, this);
    accent_border = check(f, _("The active window's border has the accent color"));
    shadows = check(f, _("Shadows under windows"));
    f.note(_("Title bar colors follow the theme (Appearance)."));

    f.space(8);
    reset_button(f.widget_x(), f.y(), wm,
                 {{B, "focus_follows_mouse"}, {B, "switcher"}, {B, "workspaces"}, {B, "restore_session"}, {B, "workspaces_per_output"}, {B, "drag_snap"}, {B, "tiling"},
                  {B, "snap_assist"}, {B, "drag_modifier"}, {B, kCornerKeys[0]}, {B, kCornerKeys[1]},
                  {B, kCornerKeys[2]}, {B, kCornerKeys[3]}, {A, "title_height"}, {A, "accent_border"},
                  {A, "shadows"}},
                 [this] { load(); });
    g->end();
    end();
    load();
  }

  Fl_Check_Button *check(Form &f, const char *label) {
    int wx, wy, ww;
    f.row("", 26, wx, wy, ww);
    Fl_Check_Button *b = new fld::CheckButton(wx, wy, ww, 26, label);
    b->callback([](Fl_Widget *, void *d) { ((WindowsPage *)d)->save(); }, this);
    return b;
  }

  // the values from wm.ini (missing keys: fl-wm's defaults)
  void load() {
    wm.ini.load(wm.path);
    ffm->value(wm.ini.get_bool(B, "focus_follows_mouse", false));
    switcher->value(wm.ini.get_bool(B, "switcher", true));
    workspaces->value(std::clamp(wm.ini.get_int(B, "workspaces", 4), 1, 9));
    restore->value(wm.ini.get_bool(B, "restore_session", false));
    per_output->value(wm.ini.get_bool(B, "workspaces_per_output", false));
    drag_snap->value(wm.ini.get_bool(B, "drag_snap", true));
    tiling->value(wm.ini.get_bool(B, "tiling", false));
    assist->value(wm.ini.get_bool(B, "snap_assist", true));
    std::string v = lower(wm.ini.get(B, "drag_modifier", "super+alt"));
    bool su = v.find("super") != std::string::npos || v.find("logo") != std::string::npos;
    bool al = v.find("alt") != std::string::npos;
    drag_mod->value(su && al ? 2 : su ? 0 : al ? 1 : 3);
    for (int i = 0; i < 4; i++) {
      std::string cur = wm.ini.get(B, kCornerKeys[i], "");
      corner[i]->value(0);
      for (int k = 1; k < kNCornerActions; k++)
        if (cur == kCornerActions[k][0]) corner[i]->value(k);
    }
    title_height->value(std::clamp(wm.ini.get_int(A, "title_height", 30), 18, 60));
    accent_border->value(wm.ini.get_bool(A, "accent_border", true));
    shadows->value(wm.ini.get_bool(A, "shadows", true));
  }

  void save() {
    wm.ini.load(wm.path);
    wm.ini.set_bool(B, "focus_follows_mouse", ffm->value());
    wm.ini.set_bool(B, "switcher", switcher->value());
    int n = (int)workspaces->value();
    if (n >= 1 && n <= 9) wm.ini.set_int(B, "workspaces", n);
    wm.ini.set_bool(B, "restore_session", restore->value());
    wm.ini.set_bool(B, "workspaces_per_output", per_output->value());
    wm.ini.set_bool(B, "drag_snap", drag_snap->value());
    wm.ini.set_bool(B, "tiling", tiling->value());
    wm.ini.set_bool(B, "snap_assist", assist->value());
    if (drag_mod->value() >= 0) wm.ini.set(B, "drag_modifier", kDragMods[drag_mod->value()]);
    for (int i = 0; i < 4; i++) {
      int k = corner[i]->value();
      if (k <= 0) wm.ini.remove(B, kCornerKeys[i]);
      else wm.ini.set(B, kCornerKeys[i], kCornerActions[k][0]);
    }
    int th = (int)title_height->value();
    if (th >= 18 && th <= 60) wm.ini.set_int(A, "title_height", th);
    wm.ini.set_bool(A, "accent_border", accent_border->value());
    wm.ini.set_bool(A, "shadows", shadows->value());
    save_wm(wm);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new WindowsPage(x, y, w, h); }

} // namespace

PageInfo page_windows() {
  return {"windows", _("Windows & Workspaces"), "preferences-system-windows", "Personal",
          _("windows workspaces virtual desktops focus follows mouse snap snapping tiling assist drag move "
            "modifier super alt hot corners title bar height shadows border alt tab switcher session restore reopen"),
          create};
}

} // namespace fld
