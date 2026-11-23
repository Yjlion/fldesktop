// fl-autorotate: turns the built-in screen with the device (convertibles and
// tablets). Reads iio-sensor-proxy's accelerometer (system bus) and sets the
// transform of the eDP / LVDS / DSI output through wlr-output-management.
// Off with [Display] AutoRotate=false in desktop.ini; quits at once when
// there is no accelerometer.
#include <gio/gio.h>
#include <glib-unix.h>
#include <wayland-client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "rotate_util.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

namespace {

struct Head {
  zwlr_output_head_v1 *head = nullptr;
  std::string name;
  bool enabled = false;
  int transform = 0;
};

wl_display *g_dpy;
zwlr_output_manager_v1 *g_mgr;
std::vector<Head *> g_heads;
uint32_t g_serial = 0;
int g_wanted = -1; // the transform to apply once the heads are known
bool g_done = false;

// the built-in screen ($FLD_ROTATE_OUTPUT names another, for tests)
bool target(const std::string &name) {
  const char *o = getenv("FLD_ROTATE_OUTPUT");
  return o && *o ? name == o : rotate::is_internal(name);
}

bool enabled() {
  fld::Ini ini;
  ini.load(fld::desktop_config_path());
  return ini.get_bool("Display", "AutoRotate", true);
}

// head events
void h_name(void *d, zwlr_output_head_v1 *, const char *n) { ((Head *)d)->name = n; }
void h_description(void *, zwlr_output_head_v1 *, const char *) {}
void h_physical_size(void *, zwlr_output_head_v1 *, int32_t, int32_t) {}
// the modes: not needed, but destroyed when they go
void m_size(void *, zwlr_output_mode_v1 *, int32_t, int32_t) {}
void m_refresh(void *, zwlr_output_mode_v1 *, int32_t) {}
void m_preferred(void *, zwlr_output_mode_v1 *) {}
void m_finished(void *, zwlr_output_mode_v1 *m) { zwlr_output_mode_v1_destroy(m); }
const zwlr_output_mode_v1_listener kMode = {m_size, m_refresh, m_preferred, m_finished};
void h_mode(void *, zwlr_output_head_v1 *, zwlr_output_mode_v1 *m) { zwlr_output_mode_v1_add_listener(m, &kMode, nullptr); }
void h_enabled(void *d, zwlr_output_head_v1 *, int32_t e) { ((Head *)d)->enabled = e; }
void h_current_mode(void *, zwlr_output_head_v1 *, zwlr_output_mode_v1 *) {}
void h_position(void *, zwlr_output_head_v1 *, int32_t, int32_t) {}
void h_transform(void *d, zwlr_output_head_v1 *, int32_t t) { ((Head *)d)->transform = t; }
void h_scale(void *, zwlr_output_head_v1 *, wl_fixed_t) {}
void h_finished(void *d, zwlr_output_head_v1 *h) {
  for (size_t i = 0; i < g_heads.size(); i++)
    if (g_heads[i] == d) {
      g_heads.erase(g_heads.begin() + (long)i);
      break;
    }
  delete (Head *)d;
  zwlr_output_head_v1_destroy(h);
}
void h_make(void *, zwlr_output_head_v1 *, const char *) {}
void h_model(void *, zwlr_output_head_v1 *, const char *) {}
void h_serial(void *, zwlr_output_head_v1 *, const char *) {}
void h_adaptive_sync(void *, zwlr_output_head_v1 *, uint32_t) {}
const zwlr_output_head_v1_listener kHead = {h_name,    h_description,  h_physical_size, h_mode,
                                           h_enabled, h_current_mode, h_position,      h_transform,
                                           h_scale,   h_finished,     h_make,          h_model,
                                           h_serial,  h_adaptive_sync};

void apply(int transform);

void mgr_head(void *, zwlr_output_manager_v1 *, zwlr_output_head_v1 *h) {
  Head *head = new Head;
  head->head = h;
  g_heads.push_back(head);
  zwlr_output_head_v1_add_listener(h, &kHead, head);
}
void mgr_done(void *, zwlr_output_manager_v1 *, uint32_t serial) {
  g_serial = serial;
  if (g_wanted >= 0) apply(g_wanted);
}
void mgr_finished(void *, zwlr_output_manager_v1 *) {}
const zwlr_output_manager_v1_listener kMgr = {mgr_head, mgr_done, mgr_finished};

void cfg_done(void *, zwlr_output_configuration_v1 *c) {
  zwlr_output_configuration_v1_destroy(c);
  g_done = true;
}
const zwlr_output_configuration_v1_listener kCfg = {cfg_done, cfg_done, cfg_done};

void apply(int transform) {
  g_wanted = transform;
  if (getenv("FLD_ROTATE_DEBUG")) fprintf(stderr, "apply %d serial %u heads %zu\n", transform, g_serial, g_heads.size());
  if (!g_mgr || !g_serial) return;
  bool change = false;
  for (Head *h : g_heads)
    if (h->enabled && target(h->name) && h->transform != transform) change = true;
  if (!change) return;
  zwlr_output_configuration_v1 *cfg = zwlr_output_manager_v1_create_configuration(g_mgr, g_serial);
  zwlr_output_configuration_v1_add_listener(cfg, &kCfg, nullptr);
  for (Head *h : g_heads) {
    if (!h->enabled) {
      zwlr_output_configuration_v1_disable_head(cfg, h->head);
      continue;
    }
    zwlr_output_configuration_head_v1 *ch = zwlr_output_configuration_v1_enable_head(cfg, h->head);
    if (target(h->name)) zwlr_output_configuration_head_v1_set_transform(ch, transform);
  }
  zwlr_output_configuration_v1_apply(cfg);
  wl_display_flush(g_dpy);
  g_wanted = -1;
}

void reg_global(void *, wl_registry *r, uint32_t name, const char *iface, uint32_t version) {
  if (!strcmp(iface, zwlr_output_manager_v1_interface.name)) {
    uint32_t v = (uint32_t)zwlr_output_manager_v1_interface.version;
    g_mgr = (zwlr_output_manager_v1 *)wl_registry_bind(r, name, &zwlr_output_manager_v1_interface,
                                                        version < v ? version : v);
    zwlr_output_manager_v1_add_listener(g_mgr, &kMgr, nullptr);
  }
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kReg = {reg_global, reg_remove};

void on_orientation(const std::string &o) {
  int t = rotate::transform_for(o);
  if (t < 0 || !enabled()) return;
  apply(t);
}

void on_props(GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *params, gpointer) {
  const char *iface;
  GVariant *changed;
  g_variant_get(params, "(&s@a{sv}@as)", &iface, &changed, nullptr);
  const char *o = nullptr;
  if (g_variant_lookup(changed, "AccelerometerOrientation", "&s", &o)) on_orientation(o);
  g_variant_unref(changed);
}

} // namespace

int main(int argc, char **argv) {
  std::string set;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
      printf("Usage: fl-autorotate [--set normal|left-up|right-up|bottom-up]\n"
             "Turns the built-in screen with the device (iio-sensor-proxy); --set turns it once.\n");
      return 0;
    }
    if (!strcmp(argv[i], "--set") && i + 1 < argc) set = argv[++i];
  }
  if (!set.empty()) {
    int t = rotate::transform_for(set);
    g_dpy = t >= 0 ? wl_display_connect(nullptr) : nullptr;
    if (!g_dpy) return 1;
    wl_registry_add_listener(wl_display_get_registry(g_dpy), &kReg, nullptr);
    wl_display_roundtrip(g_dpy);
    g_wanted = t;
    wl_display_roundtrip(g_dpy); // the heads, then apply()
    for (int i = 0; i < 50 && !g_done && g_wanted < 0; i++) wl_display_roundtrip(g_dpy);
    return 0;
  }
  GError *e = nullptr;
  GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e);
  if (!sys) return 0;
  GDBusProxy *proxy = g_dbus_proxy_new_sync(sys, G_DBUS_PROXY_FLAGS_NONE, nullptr, "net.hadess.SensorProxy",
                                            "/net/hadess/SensorProxy", "net.hadess.SensorProxy", nullptr, &e);
  g_clear_error(&e);
  GVariant *has = proxy ? g_dbus_proxy_get_cached_property(proxy, "HasAccelerometer") : nullptr;
  bool accel = has && g_variant_get_boolean(has);
  if (has) g_variant_unref(has);
  if (!accel) return 0; // nothing to follow
  g_dpy = wl_display_connect(nullptr);
  if (!g_dpy) return 1;
  wl_registry_add_listener(wl_display_get_registry(g_dpy), &kReg, nullptr);
  wl_display_roundtrip(g_dpy);
  if (!g_mgr) {
    fprintf(stderr, "fl-autorotate: the compositor has no wlr-output-management\n");
    return 1;
  }
  wl_display_roundtrip(g_dpy);
  // the sensor runs while claimed
  GVariant *r = g_dbus_proxy_call_sync(proxy, "ClaimAccelerometer", nullptr, G_DBUS_CALL_FLAGS_NONE, 3000, nullptr, &e);
  g_clear_error(&e);
  if (r) g_variant_unref(r);
  g_dbus_connection_signal_subscribe(sys, "net.hadess.SensorProxy", "org.freedesktop.DBus.Properties",
                                     "PropertiesChanged", "/net/hadess/SensorProxy", nullptr,
                                     G_DBUS_SIGNAL_FLAGS_NONE, on_props, nullptr, nullptr);
  if (GVariant *o = g_dbus_proxy_get_cached_property(proxy, "AccelerometerOrientation")) {
    on_orientation(g_variant_get_string(o, nullptr));
    g_variant_unref(o);
  }
  // Wayland events in the GLib loop
  g_unix_fd_add(wl_display_get_fd(g_dpy), G_IO_IN, [](int, GIOCondition, gpointer) -> gboolean {
    if (wl_display_dispatch(g_dpy) < 0) exit(0); // the session ended
    return G_SOURCE_CONTINUE;
  }, nullptr);
  wl_display_flush(g_dpy);
  GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(loop);
  return 0;
}
