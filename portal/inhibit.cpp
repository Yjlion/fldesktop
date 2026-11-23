// org.freedesktop.impl.portal.Inhibit: keeping the screen on (idle) and the
// computer from suspending while an application asks for it.
//
// Idle: an idle inhibitor (idle-inhibit-unstable-v1) on a surface of our own
// Wayland connection; fl-wm neither blanks nor locks while one exists.
// Suspend: a logind "sleep" inhibitor lock (block). Logging out and user
// switching are accepted and only remembered. Everything is released when
// xdg-desktop-portal closes the request. Runs on the GLib thread.
#include <fcntl.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>
#include <wayland-client.h>

#include <map>
#include <string>

#include "idle-inhibit-unstable-v1-client-protocol.h"
#include "fld/glib_bridge.h"
#include "portal.h"

namespace portal {

namespace {

enum { LOGOUT = 1, USER_SWITCH = 2, SUSPEND = 4, IDLE = 8 };

struct Wl {
  wl_display *dpy = nullptr;
  wl_compositor *comp = nullptr;
  zwp_idle_inhibit_manager_v1 *mgr = nullptr;
  bool tried = false;
} W;

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  std::string i = iface;
  if (i == wl_compositor_interface.name) W.comp = (wl_compositor *)wl_registry_bind(reg, name, &wl_compositor_interface, 1);
  else if (i == zwp_idle_inhibit_manager_v1_interface.name)
    W.mgr = (zwp_idle_inhibit_manager_v1 *)wl_registry_bind(reg, name, &zwp_idle_inhibit_manager_v1_interface, 1);
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kReg = {reg_global, reg_remove};

bool wl_ready() {
  if (W.tried) return W.mgr && W.comp;
  W.tried = true;
  W.dpy = wl_display_connect(nullptr);
  if (!W.dpy) return false;
  wl_registry *reg = wl_display_get_registry(W.dpy);
  wl_registry_add_listener(reg, &kReg, nullptr);
  wl_display_roundtrip(W.dpy);
  if (!W.mgr) g_printerr("xdg-desktop-portal-fl: the compositor has no idle-inhibit; the screen may blank\n");
  return W.mgr && W.comp;
}

struct Inhibition {
  guint reg = 0;
  wl_surface *surface = nullptr;
  zwp_idle_inhibitor_v1 *idle = nullptr;
  int sleep_fd = -1;
};
std::map<std::string, Inhibition> g_inhibitions; // by request handle

void release(const std::string &handle) {
  auto it = g_inhibitions.find(handle);
  if (it == g_inhibitions.end()) return;
  Inhibition &in = it->second;
  if (in.idle) zwp_idle_inhibitor_v1_destroy(in.idle);
  if (in.surface) wl_surface_destroy(in.surface);
  if (W.dpy) wl_display_flush(W.dpy);
  if (in.sleep_fd >= 0) close(in.sleep_fd);
  if (in.reg) g_dbus_connection_unregister_object(connection(), in.reg);
  g_inhibitions.erase(it);
}

// a logind inhibitor lock: the fd keeps it
int logind_inhibit(const char *what, const std::string &who, const std::string &why) {
  GError *e = nullptr;
  GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e);
  g_clear_error(&e);
  if (!sys) return -1;
  GUnixFDList *out = nullptr;
  GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(
      sys, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit",
      g_variant_new("(ssss)", what, who.c_str(), why.c_str(), "block"), G_VARIANT_TYPE("(h)"),
      G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr, &out, nullptr, &e);
  g_object_unref(sys);
  int fd = -1;
  if (r) {
    gint32 idx;
    g_variant_get(r, "(h)", &idx);
    fd = g_unix_fd_list_get(out, idx, nullptr);
    g_variant_unref(r);
  } else {
    g_printerr("xdg-desktop-portal-fl: logind inhibit: %s\n", e ? e->message : "?");
  }
  g_clear_error(&e);
  if (out) g_object_unref(out);
  return fd;
}

const char *kRequestXml = R"XML(
<node>
  <interface name="org.freedesktop.impl.portal.Request">
    <method name="Close"/>
  </interface>
</node>
)XML";
const char *kSessionXml = R"XML(
<node>
  <interface name="org.freedesktop.impl.portal.Session">
    <method name="Close"/>
    <signal name="Closed"/>
    <property name="version" type="u" access="read"/>
  </interface>
</node>
)XML";

void request_method(GDBusConnection *, const char *, const char *path, const char *, const char *, GVariant *,
                    GDBusMethodInvocation *inv, gpointer) {
  std::string p = path;
  g_dbus_method_invocation_return_value(inv, nullptr);
  release(p);
}

std::map<std::string, guint> g_monitors; // session handle -> registration

void monitor_method(GDBusConnection *, const char *, const char *path, const char *, const char *, GVariant *,
                    GDBusMethodInvocation *inv, gpointer) {
  g_dbus_method_invocation_return_value(inv, nullptr);
  auto it = g_monitors.find(path);
  if (it == g_monitors.end()) return;
  g_dbus_connection_unregister_object(connection(), it->second);
  g_monitors.erase(it);
}

GVariant *monitor_property(GDBusConnection *, const char *, const char *, const char *, const char *, GError **,
                           gpointer) {
  return g_variant_new_uint32(1);
}

} // namespace

void inhibit_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  if (method == "Inhibit") {
    const char *handle, *app_id, *window;
    guint32 flags;
    GVariant *options;
    g_variant_get(params, "(&o&s&su@a{sv})", &handle, &app_id, &window, &flags, &options);
    std::string reason = opt_string(options, "reason", "");
    g_variant_unref(options);
    std::string h = handle;
    release(h); // the same handle again: the new flags count
    Inhibition in;
    static GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(kRequestXml, nullptr);
    static const GDBusInterfaceVTable vt = {request_method, nullptr, nullptr, {}};
    in.reg = g_dbus_connection_register_object(connection(), handle, info->interfaces[0], &vt, nullptr, nullptr, nullptr);
    if ((flags & IDLE) && wl_ready()) {
      in.surface = wl_compositor_create_surface(W.comp);
      in.idle = zwp_idle_inhibit_manager_v1_create_inhibitor(W.mgr, in.surface);
      wl_display_flush(W.dpy);
    }
    if (flags & SUSPEND) {
      std::string who = app_name(app_id);
      in.sleep_fd = logind_inhibit("sleep", who.empty() ? "An application" : who, reason.empty() ? "Busy" : reason);
    }
    g_inhibitions[h] = in;
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  if (method == "CreateMonitor") {
    // the session state never changes here (no screensaver state to report):
    // the monitor exists so that applications can ask
    const char *handle, *session, *app_id, *window;
    g_variant_get(params, "(&o&o&s&s)", &handle, &session, &app_id, &window);
    static GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(kSessionXml, nullptr);
    static const GDBusInterfaceVTable vt = {monitor_method, monitor_property, nullptr, {}};
    guint reg = g_dbus_connection_register_object(connection(), session, info->interfaces[0], &vt, nullptr, nullptr,
                                                  nullptr);
    if (!reg) {
      g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", (guint32)OTHER));
      return;
    }
    g_monitors[session] = reg;
    g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", (guint32)SUCCESS));
    // running, not about to end; the screensaver is not active
    GVariantBuilder st;
    g_variant_builder_init(&st, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&st, "{sv}", "screensaver-active", g_variant_new_boolean(FALSE));
    g_variant_builder_add(&st, "{sv}", "session-state", g_variant_new_uint32(1));
    g_dbus_connection_emit_signal(connection(), nullptr, "/org/freedesktop/portal/desktop",
                                  "org.freedesktop.impl.portal.Inhibit", "StateChanged",
                                  g_variant_new("(o@a{sv})", session, g_variant_builder_end(&st)), nullptr);
    return;
  }
  if (method == "QueryEndResponse") {
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
}

} // namespace portal
