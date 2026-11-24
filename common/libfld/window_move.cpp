#include "fld/window_move.h"

#include <FL/Fl.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>
#include <cstdlib>
#include <cstring>

#if defined(FLTK_USE_WAYLAND) && defined(FLD_HAVE_WINDOW_PROTO)
#include <wayland-client.h>
#include "fldesktop-window-v1-client-protocol.h"
#define FLD_WINDOW_WAYLAND 1
#endif

namespace fld {

#ifdef FLD_WINDOW_WAYLAND
namespace {
fld_window_manager_v1 *g_mgr = nullptr;
bool g_tried = false;

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t) {
  if (!strcmp(iface, fld_window_manager_v1_interface.name))
    g_mgr = (fld_window_manager_v1 *)wl_registry_bind(reg, name, &fld_window_manager_v1_interface, 1);
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kRegistry = {reg_global, reg_remove};

fld_window_manager_v1 *manager() {
  if (g_tried) return g_mgr;
  wl_display *dpy = fl_wl_display();
  if (!dpy) return nullptr;
  g_tried = true;
  wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &kRegistry, nullptr);
  wl_display_roundtrip(dpy);
  return g_mgr;
}
} // namespace
#endif

bool begin_window_move(Fl_Window *win) {
  if (!win) return false;
  win = win->top_window();
  if (!win || !win->shown() || win->fullscreen_active()) return false;
#ifdef FLD_WINDOW_WAYLAND
  fld_window_manager_v1 *m = manager();
  if (!m) return false;
  wl_surface *s = fl_wl_surface(fl_wl_xid(win));
  if (!s) return false;
  fld_window_manager_v1_move(m, s);
  wl_display_flush(fl_wl_display());
  return true;
#else
  return false;
#endif
}

int window_drag_area(Fl_Widget *w, int event) {
  static int px, py;
  static Fl_Widget *tracking = nullptr;
  switch (event) {
  case FL_PUSH: {
    if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
    Fl_Window *win = w->top_window();
    if (!win || win->modal() || win->non_modal() || !win->border()) return 0; // dialogs, popups
    if (Fl::event_clicks()) { // double click: maximize / restore
      Fl::event_clicks(0);
      tracking = nullptr;
      if (win->maximize_active()) win->un_maximize();
      else win->maximize();
      return 1;
    }
    px = Fl::event_x_root();
    py = Fl::event_y_root();
    tracking = w;
    return 1;
  }
  case FL_DRAG:
    if (tracking != w) return 0;
    if (abs(Fl::event_x_root() - px) + abs(Fl::event_y_root() - py) < 5) return 1;
    tracking = nullptr;
    // the compositor takes the pointer: FLTK won't see the release
    if (begin_window_move(w->window())) Fl::pushed(nullptr);
    return 1;
  case FL_RELEASE:
    if (tracking != w) return 0;
    tracking = nullptr;
    return 1;
  }
  return 0;
}

int MenuBar::items_end() const {
  // as FLTK lays out a menu bar: titles from x()+3, each its width plus 16
  int end = x() + 3;
  // first() and next() skip submenu contents and hidden items
  for (const Fl_Menu_Item *m = menu() ? menu()->first() : nullptr; m && m->text; m = m->next()) {
    int hh;
    end += m->measure(&hh, this) + 16;
  }
  return end;
}

int MenuBar::handle(int event) {
  if (event == FL_PUSH && Fl::event_x() > items_end() + 8 && Fl::event_button() == FL_LEFT_MOUSE)
    return window_drag_area(this, event);
  if ((event == FL_DRAG || event == FL_RELEASE) && Fl::pushed() == this && window_drag_area(this, event)) return 1;
  return Fl_Menu_Bar::handle(event);
}

} // namespace fld
