// Shortcuts: the key bindings of fl-wm ([keys] in wm.ini)
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <functional>

#include "fld/desktop_entry.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

enum ArgKind { kNoArg, kCommand, kWorkspace };
struct Action {
  const char *id, *label;
  ArgKind arg;
};
// kActions[0] is the default for a new shortcut
const Action kActions[] = {
    {"exec", N_("Run a command"), kCommand},
    {"launcher", N_("Open the application menu"), kNoArg},
    {"close", N_("Close window"), kNoArg},
    {"maximize", N_("Maximize / restore"), kNoArg},
    {"minimize", N_("Minimize"), kNoArg},
    {"fullscreen", N_("Fullscreen"), kNoArg},
    {"snap-left", N_("Snap to the left"), kNoArg},
    {"snap-right", N_("Snap to the right"), kNoArg},
    {"snap-up", N_("Snap up (quarter, maximize)"), kNoArg},
    {"snap-down", N_("Snap down (quarter, restore, minimize)"), kNoArg},
    {"snap-third-left", N_("Snap to the left third"), kNoArg},
    {"snap-third-right", N_("Snap to the right third"), kNoArg},
    {"overview", N_("Overview of the windows"), kNoArg},
    {"resize-mode", N_("Resize with the keyboard"), kNoArg},
    {"cycle", N_("Next window"), kNoArg},
    {"cycle-back", N_("Previous window"), kNoArg},
    {"show-desktop", N_("Show desktop"), kNoArg},
    {"workspace", N_("Go to workspace"), kWorkspace},
    {"workspace-next", N_("Next workspace"), kNoArg},
    {"workspace-prev", N_("Previous workspace"), kNoArg},
    {"move-to-workspace", N_("Move window to workspace"), kWorkspace},
    {"move-to-workspace-next", N_("Move window to the next workspace"), kNoArg},
    {"move-to-workspace-prev", N_("Move window to the previous workspace"), kNoArg},
    {"sticky", N_("Show window on all workspaces"), kNoArg},
    {"zoom-in", N_("Zoom in (magnifier)"), kNoArg},
    {"zoom-out", N_("Zoom out"), kNoArg},
    {"zoom-reset", N_("End the zoom"), kNoArg},
    {"lock", N_("Lock the screen"), kNoArg},
    {"reload", N_("Reload configuration"), kNoArg},
    {"quit", N_("Log out (quit fl-wm)"), kNoArg},
};

// "workspace 3" -> {"workspace", "3"}
std::pair<std::string, std::string> split_action(const std::string &action) {
  std::string a = trim(action);
  size_t sp = a.find(' ');
  if (sp == std::string::npos) return {a, ""};
  return {a.substr(0, sp), trim(a.substr(sp + 1))};
}

std::string describe(const std::string &action) {
  auto [id, arg] = split_action(action);
  if (id == "exec") return "Run: " + arg;
  if (id == "portal-shortcut") return "Application shortcut (portal)";
  for (auto &x : kActions)
    if (id == x.id) return x.arg == kWorkspace ? std::string(_(x.label)) + " " + arg : _(x.label);
  return trim(action);
}

// FLTK key -> xkb keysym name
std::string keysym_name(int key) {
  switch (key) {
    case FL_Enter: return "Return";
    case FL_Tab: return "Tab";
    case FL_BackSpace: return "BackSpace";
    case FL_Escape: return "Escape";
    case FL_Delete: return "Delete";
    case FL_Insert: return "Insert";
    case FL_Home: return "Home";
    case FL_End: return "End";
    case FL_Page_Up: return "Prior";
    case FL_Page_Down: return "Next";
    case FL_Left: return "Left";
    case FL_Right: return "Right";
    case FL_Up: return "Up";
    case FL_Down: return "Down";
    case FL_Print: return "Print";
    case FL_Pause: return "Pause";
    case FL_Menu: return "Menu";
    case ' ': return "space";
  }
  if (key > FL_F && key <= FL_F_Last) return "F" + std::to_string(key - FL_F);
  if (key >= 'a' && key <= 'z') return std::string(1, (char)key);
  if (key >= '0' && key <= '9') return std::string(1, (char)key);
  static const std::pair<int, const char *> punct[] = {{'-', "minus"}, {'=', "equal"}, {',', "comma"},
                                                       {'.', "period"}, {'/', "slash"}, {';', "semicolon"},
                                                       {'\'', "apostrophe"}, {'[', "bracketleft"},
                                                       {']', "bracketright"}, {'\\', "backslash"}, {'`', "grave"}};
  for (auto &p : punct) if (key == p.first) return p.second;
  return "";
}

bool is_modifier(int key) {
  return key == FL_Shift_L || key == FL_Shift_R || key == FL_Control_L || key == FL_Control_R ||
         key == FL_Alt_L || key == FL_Alt_R || key == FL_Meta_L || key == FL_Meta_R || key == FL_Caps_Lock;
}

// a button that records the next key combination
class KeyCapture : public Fl_Button {
public:
  std::string combo;
  bool waiting = false;
  KeyCapture(int x, int y, int w, int h) : Fl_Button(x, y, w, h) { update(); }
  void update() {
    static std::string lbl;
    copy_label(waiting ? "Press the keys…" : combo.empty() ? "Click to set" : combo.c_str());
  }
  int handle(int e) override {
    if (e == FL_PUSH) { waiting = true; update(); take_focus(); return 1; }
    if (e == FL_FOCUS) return 1;
    if (e == FL_UNFOCUS) { waiting = false; update(); return 1; }
    if ((e == FL_KEYBOARD || e == FL_SHORTCUT) && waiting) {
      int k = Fl::event_key();
      if (is_modifier(k)) return 1;
      std::string name = keysym_name(k);
      if (name.empty()) return 1;
      int st = Fl::event_state();
      std::string c;
      if (st & FL_META) c += "Super+";
      if (st & FL_CTRL) c += "Ctrl+";
      if (st & FL_ALT) c += "Alt+";
      if (st & FL_SHIFT) c += "Shift+";
      combo = c + name;
      waiting = false;
      update();
      return 1;
    }
    return Fl_Button::handle(e);
  }
};

// Global shortcuts of applications (global-shortcuts.ini, written by the
// GlobalShortcuts portal): [app id] shortcut id = combo|description
void app_shortcuts_dialog() {
  std::string path = fld_config_dir() + "/global-shortcuts.ini";
  Fl_Double_Window d(560, 360, "Application Shortcuts");
  d.set_modal();
  Fl_Box note(16, 10, 528, 40, "Shortcuts that applications asked for. They work in every window; removing one "
                               "lets the application ask again.");
  note.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  note.labelsize(12);
  Fl_Hold_Browser list(16, 56, 528, 244);
  list.textsize(13);
  static int widths[] = {170, 150, 0};
  list.column_widths(widths);
  list.column_char('\t');
  list.format_char(0);
  std::vector<std::pair<std::string, std::string>> rows; // app, id
  auto fill = [&] {
    Ini ini;
    ini.load(path);
    list.clear();
    rows.clear();
    for (auto &app : ini.groups())
      for (auto &id : ini.keys(app)) {
        std::string v = ini.get(app, id);
        size_t bar = v.find('|');
        std::string combo = trim(v.substr(0, bar)), desc = bar == std::string::npos ? id : v.substr(bar + 1);
        DesktopEntry e;
        std::string name = find_application(app, e) && !e.name.empty() ? e.name : app;
        list.add((name + "\t" + (combo.empty() ? "(none)" : combo) + "\t" + desc).c_str());
        rows.push_back({app, id});
      }
    if (rows.empty()) list.add(_("No application has asked for shortcuts."));
  };
  fill();
  Fl_Button rm(16, 314, 110, 30, "Remove");
  Fl_Button close(434, 314, 110, 30, "Close");
  struct Ctx {
    Fl_Hold_Browser *list;
    std::vector<std::pair<std::string, std::string>> *rows;
    std::string path;
    std::function<void()> fill;
  } ctx{&list, &rows, path, fill};
  rm.callback([](Fl_Widget *, void *v) {
    Ctx *c = (Ctx *)v;
    int i = c->list->value() - 1;
    if (i < 0 || i >= (int)c->rows->size()) return;
    Ini ini;
    ini.load(c->path);
    ini.remove(c->rows->at((size_t)i).first, c->rows->at((size_t)i).second);
    if (ini.keys(c->rows->at((size_t)i).first).empty()) ini.remove_group(c->rows->at((size_t)i).first);
    ini.save(c->path);
    reload_wm();
    c->fill();
  }, &ctx);
  close.callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  d.end();
  d.show();
  while (d.shown()) Fl::wait();
}

// [behaviour] hot_corner_top_left= ... in wm.ini
void hot_corners_dialog() {
  static const char *keys[] = {"hot_corner_top_left", "hot_corner_top_right", "hot_corner_bottom_left",
                               "hot_corner_bottom_right"};
  static const char *labels[] = {"Top left:", "Top right:", "Bottom left:", "Bottom right:"};
  static const char *acts[] = {"", "overview", "launcher", "show-desktop", "lock", "workspace-next"};
  ConfigFile wm(wm_ini_path());
  Fl_Double_Window d(400, 230, "Hot Corners");
  d.set_modal();
  Fl_Choice *ch[4];
  for (int i = 0; i < 4; i++) {
    ch[i] = new Fl_Choice(130, 16 + i * 38, 250, 28, labels[i]);
    ch[i]->add(_("Nothing|Overview of the windows|Applications|Show the desktop|Lock the screen|Next workspace"));
    std::string cur = wm.ini.get("behaviour", keys[i], "");
    ch[i]->value(0);
    for (int k = 1; k < 6; k++)
      if (cur == acts[k]) ch[i]->value(k);
  }
  Fl_Button close(280, 186, 100, 30, "Close");
  close.callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  d.end();
  d.show();
  while (d.shown()) Fl::wait();
  wm.ini.load(wm.path);
  for (int i = 0; i < 4; i++) {
    if (ch[i]->value() <= 0) wm.ini.remove("behaviour", keys[i]);
    else wm.ini.set("behaviour", keys[i], acts[ch[i]->value()]);
    delete ch[i];
  }
  save_wm(wm);
}

class ShortcutsPage : public Fl_Group {
public:
  ConfigFile wm{wm_ini_path()};
  std::vector<std::pair<std::string, std::string>> bindings; // combo, action
  Fl_Hold_Browser *list;

  ShortcutsPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Shortcuts", cy);
    resizable(g);
    int lw = std::min(W - 48, 700);
    Fl_Check_Button *sw = new fld::CheckButton(X + 24, cy, lw, 28,
                                              _("Window switching (Alt+Tab) shows a list of the windows"));
    sw->tooltip(_("Off: each Alt+Tab raises the next window directly"));
    sw->value(wm.ini.get_bool("behaviour", "switcher", true));
    sw->callback([](Fl_Widget *w, void *d) {
      ShortcutsPage *p = (ShortcutsPage *)d;
      p->wm.ini.load(p->wm.path);
      p->wm.ini.set_bool("behaviour", "switcher", ((Fl_Check_Button *)w)->value());
      save_wm(p->wm);
    }, this);
    cy += 30;
    Fl_Check_Button *assist = new fld::CheckButton(X + 24, cy, lw, 28,
                                                  _("Snap assist: after snapping a window to a side, offer the others for the rest"));
    assist->value(wm.ini.get_bool("behaviour", "snap_assist", true));
    assist->callback([](Fl_Widget *w, void *d) {
      ShortcutsPage *p = (ShortcutsPage *)d;
      p->wm.ini.load(p->wm.path);
      p->wm.ini.set_bool("behaviour", "snap_assist", ((Fl_Check_Button *)w)->value());
      save_wm(p->wm);
    }, this);
    cy += 36;
    Fl_Spinner *ws = new Fl_Spinner(X + 24 + 160, cy, 70, 28, _("Workspaces:"));
    ws->align(FL_ALIGN_LEFT);
    ws->type(FL_INT_INPUT);
    ws->range(1, 9);
    ws->step(1);
    ws->value(std::clamp(wm.ini.get_int("behaviour", "workspaces", 4), 1, 9));
    ws->tooltip(_("How many workspaces there are (all screens switch together)"));
    ws->when(FL_WHEN_CHANGED); // a typed number applies without Enter
    ws->callback([](Fl_Widget *w, void *d) {
      ShortcutsPage *p = (ShortcutsPage *)d;
      p->wm.ini.load(p->wm.path);
      int n = (int)((Fl_Spinner *)w)->value();
      if (n < 1 || n > 9 || n == p->wm.ini.get_int("behaviour", "workspaces", 4)) return;
      p->wm.ini.set_int("behaviour", "workspaces", n);
      save_wm(p->wm);
    }, this);
    Fl_Choice *dm = new Fl_Choice(X + 24 + 420, cy, 160, 28, _("Move windows with:"));
    dm->align(FL_ALIGN_LEFT);
    dm->add(_("Super+drag|Alt+drag|Super or Alt+drag|Only the title bar"));
    {
      std::string v = wm.ini.get("behaviour", "drag_modifier", "super+alt");
      bool su = v.find("super") != std::string::npos, al = v.find("alt") != std::string::npos;
      dm->value(su && al ? 2 : su ? 0 : al ? 1 : 3);
    }
    dm->tooltip(_("Hold this key and drag with the left button to move a window, with the right button to resize it"));
    dm->callback([](Fl_Widget *w, void *d) {
      static const char *vals[] = {"super", "alt", "super+alt", "none"};
      ShortcutsPage *p = (ShortcutsPage *)d;
      p->wm.ini.load(p->wm.path);
      p->wm.ini.set("behaviour", "drag_modifier", vals[((Fl_Choice *)w)->value()]);
      save_wm(p->wm);
    }, this);
    cy += 36;
    // the buttons: Add / Edit / Remove on the left, the rest on the right;
    // the right group goes on a second row when both don't fit
    fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
    auto bw = [](const char *l) { return std::max(72, (int)fl_width(l) + 20); };
    const char *left_l[] = {"Add…", "Edit…", "Remove"};
    const char *right_l[] = {"Application Shortcuts…", "Hot Corners…", "Reset to Defaults"};
    const int gap = 6;
    int left_w = -gap, right_w = -gap;
    for (const char *l : left_l) left_w += bw(l) + gap;
    for (const char *l : right_l) right_w += bw(l) + gap;
    bool two_rows = left_w + 24 + right_w > lw;
    int rows_h = two_rows ? 72 : 36;
    list = new Fl_Hold_Browser(X + 24, cy, lw, H - (cy - Y) - 28 - rows_h);
    list->textsize(13);
    static int widths[] = {220, 0};
    list->column_widths(widths);
    list->column_char('\t');
    list->callback([](Fl_Widget *, void *d) { if (Fl::event_clicks()) ((ShortcutsPage *)d)->edit(false); }, this);
    int by = Y + H - 14 - rows_h;
    int bx = X + 24;
    auto place = [&](const char *l, int &x, int y) {
      Fl_Button *b = new Fl_Button(x, y, bw(l), 30, l);
      x += bw(l) + gap;
      return b;
    };
    Fl_Button *add = place(left_l[0], bx, by);
    add->callback([](Fl_Widget *, void *d) { ((ShortcutsPage *)d)->edit(true); }, this);
    Fl_Button *ed = place(left_l[1], bx, by);
    ed->callback([](Fl_Widget *, void *d) { ((ShortcutsPage *)d)->edit(false); }, this);
    Fl_Button *rm = place(left_l[2], bx, by);
    rm->callback([](Fl_Widget *, void *d) {
      ShortcutsPage *p = (ShortcutsPage *)d;
      int i = p->list->value() - 1;
      if (i < 0) return;
      p->bindings.erase(p->bindings.begin() + i);
      p->fill();
      p->save();
    }, this);
    int ry = two_rows ? by + 36 : by;
    int rx = two_rows ? X + 24 : X + 24 + lw - right_w;
    Fl_Button *apps = place(right_l[0], rx, ry);
    apps->tooltip(_("Global shortcuts that applications asked for (through the desktop portal)"));
    apps->callback([](Fl_Widget *, void *) { app_shortcuts_dialog(); });
    Fl_Button *corners = place(right_l[1], rx, ry);
    corners->tooltip(_("What pushing the pointer into a corner of the screen does"));
    corners->callback([](Fl_Widget *, void *) { hot_corners_dialog(); });
    Fl_Button *reset = place(right_l[2], rx, ry);
    fld::set_context_menu(list, [add, ed, rm, reset](fld::Menu &m) {
      m.button(add).button(ed).button(rm).divider().button(reset);
    });
    reset->callback([](Fl_Widget *, void *d) {
      if (fl_choice("%s", _("Cancel"), _("Reset"), nullptr, _("Replace all shortcuts with the defaults?")) != 1) return;
      ShortcutsPage *p = (ShortcutsPage *)d;
      p->load_defaults();
      p->fill();
      p->save();
    }, this);
    g->end();
    end();
    load();
    fill();
  }

  void load_defaults() {
    bindings.clear();
    Ini ex;
    for (std::string p : {fld_data_dir() + "/wm.ini.example", std::string("/usr/share/fldesktop/wm.ini.example")})
      if (ex.load(p)) break;
    for (auto &k : ex.keys("keys"))
      if (k != "defaults") bindings.push_back({k, ex.get("keys", k)});
  }

  void load() {
    // the built-in list, with the user's lines applied on top (as fl-wm does)
    bool replace = !wm.ini.get_bool("keys", "defaults", true);
    if (!replace) load_defaults();
    for (auto &k : wm.ini.keys("keys")) {
      if (k == "defaults") continue;
      bool found = false;
      for (auto &b : bindings)
        if (lower(b.first) == lower(k)) { b.second = wm.ini.get("keys", k); found = true; }
      if (!found) bindings.push_back({k, wm.ini.get("keys", k)});
    }
  }

  void fill() {
    int sel = list->value();
    list->clear();
    for (auto &b : bindings) list->add(escape_at(b.first + "\t" + describe(b.second)).c_str());
    list->value(std::min(sel, list->size()));
  }

  void edit(bool add) {
    int idx = list->value() - 1;
    if (!add && idx < 0) return;
    Fl_Double_Window w(460, 220, add ? "Add Shortcut" : "Edit Shortcut");
    new Fl_Box(10, 16, 110, 30, _("Keys:"));
    KeyCapture *keys = new KeyCapture(120, 16, 330, 30);
    Fl_Choice *action = new Fl_Choice(120, 56, 330, 30, _("Action:"));
    for (auto &a : kActions) action->add(_(a.label));
    Fl_Input *cmd = new Fl_Input(120, 96, 330, 30, _("Command:"));
    std::string cur = add ? "exec " : bindings[idx].second;
    if (!add) {
      keys->combo = bindings[idx].first;
      keys->update();
    }
    auto [cur_id, cur_arg] = split_action(cur);
    int ai = 0;
    for (size_t i = 0; i < sizeof kActions / sizeof kActions[0]; i++)
      if (cur_id == kActions[i].id) ai = (int)i;
    action->value(ai);
    if (kActions[ai].arg != kNoArg) cmd->value(cur_arg.c_str());
    // the text field holds the command or the workspace number
    auto sync = [](Fl_Widget *, void *d) {
      Fl_Input *c = (Fl_Input *)d;
      Fl_Choice *a = (Fl_Choice *)c->parent()->child(2);
      ArgKind k = kActions[std::max(0, a->value())].arg;
      c->label(k == kWorkspace ? "Workspace:" : "Command:");
      if (k != kNoArg) c->activate(); else c->deactivate();
      c->parent()->redraw();
    };
    action->callback(sync, cmd);
    sync(nullptr, cmd);
    Fl_Button *cancel = new Fl_Button(240, 176, 100, 30, _("Cancel"));
    Fl_Return_Button *ok = new Fl_Return_Button(350, 176, 100, 30, _("OK"));
    static int result;
    result = 0;
    cancel->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
    ok->callback([](Fl_Widget *b, void *) { result = 1; b->window()->hide(); });
    w.end();
    w.set_modal();
    w.show();
    while (w.shown()) Fl::wait();
    if (!result || keys->combo.empty()) return;
    const Action &chosen = kActions[std::max(0, action->value())];
    std::string act = chosen.id;
    if (chosen.arg == kCommand) {
      if (!*cmd->value()) return;
      act += std::string(" ") + cmd->value();
    } else if (chosen.arg == kWorkspace) {
      int n = atoi(cmd->value());
      if (n < 1 || n > 9) {
        fl_alert("%s", _("The workspace is a number from 1 to 9."));
        return;
      }
      act += " " + std::to_string(n);
    }
    // one action per key combination
    for (size_t i = 0; i < bindings.size(); i++)
      if ((add || (int)i != idx) && lower(bindings[i].first) == lower(keys->combo)) {
        if (fl_choice(_("%s is already used for \"%s\". Replace it?"), _("Cancel"), _("Replace"), nullptr,
                      keys->combo.c_str(), describe(bindings[i].second).c_str()) != 1)
          return;
        bindings.erase(bindings.begin() + i);
        if (!add && (int)i < idx) idx--;
        break;
      }
    if (add) bindings.push_back({keys->combo, act});
    else bindings[idx] = {keys->combo, act};
    fill();
    save();
  }

  void save() {
    wm.ini.load(wm.path);
    wm.ini.remove_group("keys");
    wm.ini.set("keys", "defaults", "false");
    for (auto &b : bindings) wm.ini.set("keys", b.first, b.second);
    save_wm(wm);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new ShortcutsPage(x, y, w, h); }

} // namespace

PageInfo page_shortcuts() {
  return {"shortcuts", _("Shortcuts"), "preferences-desktop-keyboard-shortcuts", "Hardware",
          _("keyboard shortcuts hotkeys bindings keys super launcher"), create};
}

} // namespace fld
