// Panel & Desktop: panel position, quick launch, clock, keyboard button, desktop icons
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Return_Button.H>

#include "fld/menus.h"
#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

const char *kClockFormats[][2] = {
    {"%a %d %b  %H:%M", "Mon 16 Nov  14:05"}, {"%H:%M", "14:05"}, {"%a %H:%M", "Mon 14:05"},
    {"%I:%M %p", "02:05 PM"}, {"%a %d %b  %I:%M %p", "Mon 16 Nov  02:05 PM"},
    {"%a %d %b  %H:%M:%S", "Mon 16 Nov  14:05:09"}, {"%Y-%m-%d %H:%M", "2026-11-16 14:05"},
};

class PanelPage : public Fl_Group {
public:
  ConfigFile panel{panel_ini_path()}, wm{wm_ini_path()}, desk{desktop_ini_path()};
  Fl_Choice *position, *clock, *kbd;
  Fl_Hold_Browser *quick;
  Fl_Check_Button *icons, *quick_settings;
  std::vector<std::string> quick_ids;

  PanelPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Panel & Desktop"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    f.heading(_("Panel"));
    f.row(_("Position:"), 28, wx, wy, ww);
    position = new Fl_Choice(wx, wy, 200, 28);
    position->add(_("Bottom"));
    position->add(_("Top"));
    position->value(lower(wm.ini.get("behaviour", "panel_position", "bottom")) == "top" ? 1 : 0);
    position->callback([](Fl_Widget *, void *d) { ((PanelPage *)d)->save(); }, this);
    f.row(_("Clock:"), 28, wx, wy, ww);
    clock = new Fl_Choice(wx, wy, 260, 28);
    std::string cur = panel.ini.get("Clock", "Format", kClockFormats[0][0]);
    for (size_t i = 0; i < sizeof kClockFormats / sizeof kClockFormats[0]; i++) {
      char buf[64];
      time_t now = time(nullptr);
      struct tm tm;
      localtime_r(&now, &tm);
      strftime(buf, sizeof buf, kClockFormats[i][0], &tm);
      clock->add(escape_at(buf).c_str());
      if (cur == kClockFormats[i][0]) clock->value((int)i);
    }
    if (clock->value() < 0) clock->value(0);
    clock->callback([](Fl_Widget *, void *d) { ((PanelPage *)d)->save(); }, this);
    f.row(_("Keyboard button:"), 28, wx, wy, ww);
    kbd = new Fl_Choice(wx, wy, 260, 28);
    kbd->add(_("When there is no keyboard"));
    kbd->add(_("Always"));
    kbd->add(_("Never"));
    std::string km = panel.ini.get("Panel", "KeyboardButton", "auto");
    kbd->value(km == "always" ? 1 : km == "never" ? 2 : 0);
    kbd->callback([](Fl_Widget *, void *d) { ((PanelPage *)d)->save(); }, this);
    f.row("", 26, wx, wy, ww);
    quick_settings = new fld::CheckButton(wx, wy, ww, 26, _("Quick settings button (Wi-Fi, Bluetooth, Do Not Disturb, …)"));
    quick_settings->value(panel.ini.get_bool("Panel", "QuickSettings", true));
    quick_settings->callback([](Fl_Widget *, void *d) { ((PanelPage *)d)->save(); }, this);

    f.row(_("Quick launch:"), 150, wx, wy, ww);
    quick = new Fl_Hold_Browser(wx, wy, ww - 110, 150);
    quick->textsize(13);
    quick_ids = split(panel.ini.get("Panel", "QuickLaunch", "fl-files;fl-term;fl-edit"), ';');
    Fl_Button *add = new Fl_Button(wx + ww - 100, wy, 100, 28, _("Add…"));
    add->callback([](Fl_Widget *, void *d) { ((PanelPage *)d)->add_app(); }, this);
    Fl_Button *rm = new Fl_Button(wx + ww - 100, wy + 34, 100, 28, _("Remove"));
    rm->callback([](Fl_Widget *, void *d) {
      PanelPage *p = (PanelPage *)d;
      int i = p->quick->value() - 1;
      if (i < 0 || i >= (int)p->quick_ids.size()) return;
      p->quick_ids.erase(p->quick_ids.begin() + i);
      p->fill_quick();
      p->save();
    }, this);
    Fl_Button *up = new Fl_Button(wx + ww - 100, wy + 68, 100, 28, _("Move Up"));
    up->callback([](Fl_Widget *, void *d) { ((PanelPage *)d)->move(-1); }, this);
    Fl_Button *down = new Fl_Button(wx + ww - 100, wy + 102, 100, 28, _("Move Down"));
    fld::set_context_menu(quick, [add, rm, up, down](fld::Menu &m) {
      m.button(add).button(rm).divider().button(up).button(down);
    });
    down->callback([](Fl_Widget *, void *d) { ((PanelPage *)d)->move(1); }, this);
    fill_quick();

    f.heading(_("Desktop"));
    f.row("", 26, wx, wy, ww);
    icons = new fld::CheckButton(wx, wy, ww, 26, _("Show icons on the desktop"));
    icons->value(desk.ini.get_bool("Desktop", "ShowIcons", true));
    icons->callback([](Fl_Widget *, void *d) { ((PanelPage *)d)->save(); }, this);
    f.note(_("The wallpaper is set on the Appearance page."));
    f.space(8);
    reset_page_button(f.widget_x(), f.y(), {{panel_ini_path(), {{"Clock", "Format"}, {"Panel", "KeyboardButton"}, {"Panel", "QuickLaunch"},
                                                             {"Panel", "QuickSettings"}}},
                       {desktop_ini_path(), {{"Desktop", "ShowIcons"}}},
                       {wm_ini_path(), {{"behaviour", "panel_position"}}}});
    g->end();
    end();
  }

  void fill_quick() {
    int sel = quick->value();
    quick->clear();
    for (auto &id : quick_ids) {
      DesktopEntry e;
      std::string label = find_application(id, e) ? e.name : id + " (not installed)";
      quick->add(escape_at(label).c_str());
      quick->icon(quick->size(), icon_first({e.icon, "application-x-executable"}, 20));
    }
    quick->value(std::min(sel, quick->size()));
  }

  void move(int d) {
    int i = quick->value() - 1, j = i + d;
    if (i < 0 || j < 0 || j >= (int)quick_ids.size()) return;
    std::swap(quick_ids[i], quick_ids[j]);
    fill_quick();
    quick->value(j + 1);
    save();
  }

  void add_app() {
    auto apps = all_applications();
    Fl_Double_Window w(380, 440, "Add to Quick Launch");
    Fl_Hold_Browser *b = new Fl_Hold_Browser(10, 10, 360, 380);
    for (auto &a : apps) {
      b->add(escape_at(a.name).c_str());
      b->icon(b->size(), icon_first({a.icon, "application-x-executable"}, 20));
    }
    Fl_Button *cancel = new Fl_Button(160, 400, 100, 30, _("Cancel"));
    Fl_Return_Button *ok = new Fl_Return_Button(270, 400, 100, 30, _("Add"));
    static int result;
    result = 0;
    cancel->callback([](Fl_Widget *x, void *) { x->window()->hide(); });
    ok->callback([](Fl_Widget *x, void *) { result = 1; x->window()->hide(); });
    b->callback([](Fl_Widget *x, void *) { if (Fl::event_clicks()) { result = 1; x->window()->hide(); } });
    w.end();
    w.set_modal();
    w.show();
    while (w.shown()) Fl::wait();
    int line = b->value();
    if (!result || line < 1) return;
    std::string id = apps[line - 1].id;
    if (ends_with(id, ".desktop")) id = id.substr(0, id.size() - 8);
    quick_ids.push_back(id);
    fill_quick();
    save();
  }

  void save() {
    panel.ini.load(panel.path);
    wm.ini.load(wm.path);
    desk.ini.load(desk.path);
    panel.ini.set("Clock", "Format", kClockFormats[std::max(0, clock->value())][0]);
    const char *km[] = {"auto", "always", "never"};
    panel.ini.set("Panel", "KeyboardButton", km[std::max(0, kbd->value())]);
    panel.ini.set("Panel", "QuickLaunch", join(quick_ids, ";"));
    panel.ini.set_bool("Panel", "QuickSettings", quick_settings->value() != 0);
    panel.save();
    std::string pos = position->value() == 1 ? "top" : "bottom";
    if (lower(wm.ini.get("behaviour", "panel_position", "bottom")) != pos) {
      wm.ini.set("behaviour", "panel_position", pos);
      save_wm(wm);
    }
    desk.ini.set_bool("Desktop", "ShowIcons", icons->value());
    desk.save();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new PanelPage(x, y, w, h); }

} // namespace

PageInfo page_panel() {
  return {"panel", _("Panel & Desktop"), "preferences-desktop", "Personal",
          _("taskbar panel clock quick launch desktop icons position top bottom"), create};
}

} // namespace fld
