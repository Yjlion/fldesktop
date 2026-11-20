// Shared parts of fl-panel: the popup window and the helpers the indicator
// files (panel_*.cpp) use.
#pragma once
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include <cstdint>
#include <functional>

#include "fld/menus.h"
#include "fld/theme.h"
#include "fld/widgets.h"

static const int PANEL_H = 38;

Fl_Color panel_bg();
Fl_Color panel_fg();
Fl_Color panel_hover();
// Lays out the panel again (after an indicator was shown or hidden).
void relayout();
// A right-click menu for a panel widget: fill adds the items when it is
// shown. The left-click action no longer runs on a right click.
void panel_context_menu(Fl_Widget *w, std::function<void(fld::Menu &)> fill);
// Items most panel menus end with (Panel Settings…)
void panel_menu_items(fld::Menu &m);

// A flat panel button with a theme icon.
fld::FlatButton *make_btn(int w, const char *icon, const char *tip, Fl_Callback *cb);

// A popup window anchored to a panel widget (an xdg_popup on Wayland).
// A popup above the panel (calendar, volume). It is a borderless toplevel
// titled "fldesktop.panel-popup": fl-wm places it at the pointer next to the
// panel, keeps it out of the task list and closes it when another window is
// focused. (FLTK only creates xdg_popups for its own menus.)
class PanelPopup : public Fl_Double_Window {
public:
  PanelPopup(int w, int h) : Fl_Double_Window(w, h, "fldesktop.panel-popup") {
    box(FL_BORDER_BOX);
    color(fld::palette().bg2);
    border(0);
    callback([](Fl_Widget *w, void *) { w->hide(); });
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD && Fl::event_key() == FL_Escape) {
      hide();
      return 1;
    }
    return Fl_Double_Window::handle(e);
  }
  void popup_at(Fl_Widget *, bool) {
    show();
    // FLTK ignores xdg_toplevel.close for borderless windows, so the popup
    // closes itself once the program has lost the keyboard focus (fl-wm moves
    // it away when another window is clicked)
    bool had_focus = false;
    while (shown()) {
      Fl::wait();
      if (Fl::focus()) had_focus = true;
      else if (had_focus) hide();
    }
  }
};

// Indicators in their own files. Each create function makes the (hidden)
// button inside the panel window that is being built and starts watching its
// service; the button shows itself when there is something to show.
fld::FlatButton *notify_indicator_create();     // panel_notify.cpp
fld::FlatButton *printer_indicator_create();    // panel_printers.cpp
fld::FlatButton *bluetooth_indicator_create();  // panel_bluetooth.cpp
fld::FlatButton *nightlight_indicator_create(); // panel_nightlight.cpp
// the "Night Light" toggle for the panel's menu (nothing without fl-nightlight)
void nightlight_menu_items(fld::Menu &m);
fld::FlatButton *clipboard_indicator_create();  // panel_clipboard.cpp
// panel_pager.cpp: the workspace pager (hidden until fl-wm offers workspaces);
// pager_bind takes the ext_workspace_manager_v1 global from the registry
struct wl_registry;
Fl_Widget *pager_create();
bool pager_bind(wl_registry *reg, uint32_t name, const char *iface, uint32_t version);

// panel_power.cpp: drives the battery button from UPower (also power modes and
// low battery warnings). While UPower is not running the panel reads /sys.
void power_indicator_start(fld::FlatButton *battery);
bool power_indicator_active();
// panel_drives.cpp: mounts removable drives as they are plugged in.
void drives_start();
// panel_network.cpp: the network button's popup and right-click menu
void network_popup(Fl_Widget *button);
void network_context_menu(fld::Menu &m);
