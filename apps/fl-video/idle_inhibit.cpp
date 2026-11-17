#include "idle_inhibit.h"

#include <FL/Fl_Window.H>
#include <FL/platform.H>
#include <cstring>
#include <wayland-client.h>

#include "idle-inhibit-unstable-v1-client-protocol.h"

namespace idle {
namespace {

zwp_idle_inhibit_manager_v1 *g_mgr = nullptr;
zwp_idle_inhibitor_v1 *g_inhibitor = nullptr;
bool g_tried = false;

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t) {
  if (!strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name))
    g_mgr = (zwp_idle_inhibit_manager_v1 *)wl_registry_bind(reg, name, &zwp_idle_inhibit_manager_v1_interface, 1);
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kReg = {reg_global, reg_remove};

} // namespace

void inhibit(Fl_Window *win, bool on) {
  wl_display *dpy = fl_wl_display();
  if (!dpy || !win || !win->shown()) return;
  if (!g_tried) {
    g_tried = true;
    wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &kReg, nullptr);
    wl_display_roundtrip(dpy);
    wl_registry_destroy(reg);
  }
  if (!g_mgr) return;
  if (on && !g_inhibitor) {
    wl_surface *surf = fl_wl_surface(fl_wl_xid(win));
    if (surf) g_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(g_mgr, surf);
  } else if (!on && g_inhibitor) {
    zwp_idle_inhibitor_v1_destroy(g_inhibitor);
    g_inhibitor = nullptr;
  }
  wl_display_flush(dpy);
}

} // namespace idle
