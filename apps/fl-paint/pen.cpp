#include "pen.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <cstring>
#include <wayland-client.h>

#ifdef FLD_TABLET_UNSTABLE
#include "tablet-unstable-v2-client-protocol.h"
#else
#include "tablet-v2-client-protocol.h"
#endif

namespace pen {

namespace {

Fl_Window *g_win;
zwp_tablet_manager_v2 *g_mgr;
wl_seat *g_seat;
bool g_in, g_down;
double g_x, g_y, g_pressure = 1.0;
bool g_moved;

// an FLTK mouse event at the pen's place in the window
void send(int event) {
  if (!g_win || !g_win->shown()) return;
  float s = Fl::screen_scale(g_win->screen_num());
  if (s <= 0) s = 1;
  Fl::e_x = (int)(g_x / s);
  Fl::e_y = (int)(g_y / s);
  Fl::e_x_root = g_win->x() + Fl::e_x;
  Fl::e_y_root = g_win->y() + Fl::e_y;
  if (event == FL_PUSH || event == FL_RELEASE) {
    Fl::e_keysym = FL_Button + FL_LEFT_MOUSE;
    Fl::e_is_click = 0;
    Fl::e_clicks = 0;
  }
  Fl::e_state = (Fl::e_state & ~FL_BUTTONS) | (g_down && event != FL_RELEASE ? FL_BUTTON1 : 0);
  Fl::handle(event, g_win);
}

// ---- tool
void t_type(void *, zwp_tablet_tool_v2 *, uint32_t) {}
void t_serial(void *, zwp_tablet_tool_v2 *, uint32_t, uint32_t) {}
void t_hw_id(void *, zwp_tablet_tool_v2 *, uint32_t, uint32_t) {}
void t_capability(void *, zwp_tablet_tool_v2 *, uint32_t) {}
void t_done(void *, zwp_tablet_tool_v2 *) {}
void t_removed(void *, zwp_tablet_tool_v2 *t) { zwp_tablet_tool_v2_destroy(t); }
void t_proximity_in(void *, zwp_tablet_tool_v2 *, uint32_t, zwp_tablet_v2 *, wl_surface *surface) {
  // our window (its surface) or one of its subwindows
  g_in = g_win && g_win->shown() && surface == fl_wl_surface(fl_wl_xid(g_win));
}
void t_proximity_out(void *, zwp_tablet_tool_v2 *) {
  if (g_down) {
    g_down = false;
    send(FL_RELEASE);
  }
  g_in = false;
}
void t_down(void *, zwp_tablet_tool_v2 *, uint32_t) {
  if (!g_in) return;
  g_down = true;
  send(FL_PUSH);
}
void t_up(void *, zwp_tablet_tool_v2 *) {
  if (!g_down) return;
  g_down = false;
  send(FL_RELEASE);
  g_pressure = 1.0;
}
void t_motion(void *, zwp_tablet_tool_v2 *, wl_fixed_t x, wl_fixed_t y) {
  g_x = wl_fixed_to_double(x);
  g_y = wl_fixed_to_double(y);
  g_moved = true;
}
void t_pressure(void *, zwp_tablet_tool_v2 *, uint32_t p) { g_pressure = p / 65535.0; }
void t_distance(void *, zwp_tablet_tool_v2 *, uint32_t) {}
void t_tilt(void *, zwp_tablet_tool_v2 *, wl_fixed_t, wl_fixed_t) {}
void t_rotation(void *, zwp_tablet_tool_v2 *, wl_fixed_t) {}
void t_slider(void *, zwp_tablet_tool_v2 *, int32_t) {}
void t_wheel(void *, zwp_tablet_tool_v2 *, wl_fixed_t, int32_t) {}
void t_button(void *, zwp_tablet_tool_v2 *, uint32_t, uint32_t, uint32_t) {}
void t_frame(void *, zwp_tablet_tool_v2 *, uint32_t) {
  if (g_in && g_moved) send(g_down ? FL_DRAG : FL_MOVE);
  g_moved = false;
}
const zwp_tablet_tool_v2_listener kTool = {
    t_type,   t_serial, t_hw_id,      t_capability, t_done,     t_removed,  t_proximity_in,
    t_proximity_out, t_down, t_up,     t_motion,     t_pressure, t_distance, t_tilt,
    t_rotation, t_slider, t_wheel,    t_button,     t_frame};  // the 19 events, in protocol order

// ---- tablet seat: tablets and pads are not needed, only tools
void s_tablet(void *, zwp_tablet_seat_v2 *, zwp_tablet_v2 *) {}
void s_tool(void *, zwp_tablet_seat_v2 *, zwp_tablet_tool_v2 *tool) { zwp_tablet_tool_v2_add_listener(tool, &kTool, nullptr); }
void s_pad(void *, zwp_tablet_seat_v2 *, zwp_tablet_pad_v2 *) {}
const zwp_tablet_seat_v2_listener kSeat = {s_tablet, s_tool, s_pad};

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  if (!strcmp(iface, zwp_tablet_manager_v2_interface.name) && !g_mgr)
    g_mgr = (zwp_tablet_manager_v2 *)wl_registry_bind(reg, name, &zwp_tablet_manager_v2_interface, 1);
  else if (!strcmp(iface, wl_seat_interface.name) && !g_seat)
    g_seat = (wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kReg = {reg_global, reg_remove};

}  // namespace

bool start(Fl_Window *win) {
  wl_display *dpy = fl_wl_display();
  if (!dpy || g_mgr) return g_mgr != nullptr;
  g_win = win;
  wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &kReg, nullptr);
  wl_display_roundtrip(dpy);
  if (!g_mgr || !g_seat) return false;
  zwp_tablet_seat_v2 *seat = zwp_tablet_manager_v2_get_tablet_seat(g_mgr, g_seat);
  zwp_tablet_seat_v2_add_listener(seat, &kSeat, nullptr);
  wl_display_roundtrip(dpy);
  return true;
}

bool active() { return g_down; }
double pressure() { return g_down ? g_pressure : 1.0; }

}  // namespace pen
