// xdg-desktop-portal-fl: the fldesktop backend for xdg-desktop-portal
//
// Owns org.freedesktop.impl.portal.desktop.fl and implements FileChooser,
// AppChooser, Settings, Screenshot, Access, Account, Notification,
// Wallpaper and ScreenCast at /org/freedesktop/portal/desktop. xdg-desktop-portal picks it
// for XDG_CURRENT_DESKTOP=fldesktop (see fldesktop-portals.conf).
#include <FL/Fl.H>

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "fld/glib_bridge.h"
#include "fld/theme.h"
#include "portal.h"

namespace portal {

namespace {

const char *kXml = R"XML(
<node>
  <interface name="org.freedesktop.impl.portal.FileChooser">
    <method name="OpenFile">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SaveFile">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SaveFiles">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
  </interface>
  <interface name="org.freedesktop.impl.portal.AppChooser">
    <method name="ChooseApplication">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="as" name="choices" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="UpdateChoices">
      <arg type="o" name="handle" direction="in"/><arg type="as" name="choices" direction="in"/>
    </method>
  </interface>
  <interface name="org.freedesktop.impl.portal.Settings">
    <method name="ReadAll">
      <arg type="as" name="namespaces" direction="in"/><arg type="a{sa{sv}}" name="value" direction="out"/>
    </method>
    <method name="Read">
      <arg type="s" name="namespace" direction="in"/><arg type="s" name="key" direction="in"/>
      <arg type="v" name="value" direction="out"/>
    </method>
    <signal name="SettingChanged">
      <arg type="s" name="namespace"/><arg type="s" name="key"/><arg type="v" name="value"/>
    </signal>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="org.freedesktop.impl.portal.Screenshot">
    <method name="Screenshot">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="PickColor">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="org.freedesktop.impl.portal.Access">
    <method name="AccessDialog">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="title" direction="in"/>
      <arg type="s" name="subtitle" direction="in"/><arg type="s" name="body" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
  </interface>
  <interface name="org.freedesktop.impl.portal.Account">
    <method name="GetUserInformation">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="window" direction="in"/><arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
  </interface>
  <interface name="org.freedesktop.impl.portal.Notification">
    <method name="AddNotification">
      <arg type="s" name="app_id" direction="in"/><arg type="s" name="id" direction="in"/>
      <arg type="a{sv}" name="notification" direction="in"/>
    </method>
    <method name="RemoveNotification">
      <arg type="s" name="app_id" direction="in"/><arg type="s" name="id" direction="in"/>
    </method>
    <signal name="ActionInvoked">
      <arg type="s" name="app_id"/><arg type="s" name="id"/><arg type="s" name="action"/>
      <arg type="av" name="parameter"/>
    </signal>
    <property name="SupportedOptions" type="a{sv}" access="read"/>
  </interface>
  <interface name="org.freedesktop.impl.portal.ScreenCast">
    <method name="CreateSession">
      <arg type="o" name="handle" direction="in"/><arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/><arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="SelectSources">
      <arg type="o" name="handle" direction="in"/><arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/><arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <method name="Start">
      <arg type="o" name="handle" direction="in"/><arg type="o" name="session_handle" direction="in"/>
      <arg type="s" name="app_id" direction="in"/><arg type="s" name="parent_window" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/><arg type="a{sv}" name="results" direction="out"/>
    </method>
    <property name="AvailableSourceTypes" type="u" access="read"/>
    <property name="AvailableCursorModes" type="u" access="read"/>
    <property name="version" type="u" access="read"/>
  </interface>
  <interface name="org.freedesktop.impl.portal.Wallpaper">
    <method name="SetWallpaperURI">
      <arg type="o" name="handle" direction="in"/><arg type="s" name="app_id" direction="in"/>
      <arg type="s" name="parent_window" direction="in"/><arg type="s" name="uri" direction="in"/>
      <arg type="a{sv}" name="options" direction="in"/>
      <arg type="u" name="response" direction="out"/>
    </method>
  </interface>
</node>
)XML";

const char *kRequestXml = R"XML(
<node>
  <interface name="org.freedesktop.impl.portal.Request">
    <method name="Close"/>
  </interface>
</node>
)XML";

const char *kPath = "/org/freedesktop/portal/desktop";

GDBusConnection *g_conn = nullptr;
GDBusNodeInfo *g_request_info = nullptr;
std::mutex g_req_mutex;
struct Req {
  guint reg = 0;
  std::function<void()> close;
};
std::map<std::string, Req> g_requests;
bool g_replace = false;

void method_call(GDBusConnection *, const char *, const char *, const char *iface, const char *method,
                 GVariant *params, GDBusMethodInvocation *inv, gpointer) {
  std::string i = iface, m = method;
  const std::string p = "org.freedesktop.impl.portal.";
  if (i == p + "FileChooser") filechooser_call(m, params, inv);
  else if (i == p + "AppChooser") appchooser_call(m, params, inv);
  else if (i == p + "Settings") settings_call(m, params, inv);
  else if (i == p + "Screenshot") screenshot_call(m, params, inv);
  else if (i == p + "Access") access_call(m, params, inv);
  else if (i == p + "Account") account_call(m, params, inv);
  else if (i == p + "Notification") notification_call(m, params, inv);
  else if (i == p + "Wallpaper") wallpaper_call(m, params, inv);
  else if (i == p + "ScreenCast") screencast_call(m, params, inv);
  else g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownInterface", iface);
}

GVariant *get_property(GDBusConnection *, const char *, const char *, const char *iface, const char *prop, GError **,
                       gpointer) {
  std::string i = iface, p = prop;
  if (i == "org.freedesktop.impl.portal.ScreenCast") return screencast_property(p);
  if (p == "version") return g_variant_new_uint32(i.find("Settings") != std::string::npos ? 2 : 2);
  if (p == "SupportedOptions") return g_variant_new("a{sv}", nullptr);
  return nullptr;
}

void request_call(GDBusConnection *, const char *, const char *path, const char *, const char *method, GVariant *,
                  GDBusMethodInvocation *inv, gpointer) {
  std::function<void()> close;
  {
    std::lock_guard<std::mutex> l(g_req_mutex);
    auto it = g_requests.find(path);
    if (it != g_requests.end()) close = it->second.close;
  }
  if (close) fld::ui_call(close);
  g_dbus_method_invocation_return_value(inv, nullptr);
}

} // namespace

GDBusConnection *connection() { return g_conn; }

void reply(GDBusMethodInvocation *inv, Response r, GVariant *results) {
  if (!results) results = g_variant_new("a{sv}", nullptr);
  GVariant *v = g_variant_ref_sink(g_variant_new("(u@a{sv})", (guint32)r, results));
  fld::glib_call([inv, v] {
    g_dbus_method_invocation_return_value(inv, v);
    g_variant_unref(v);
  });
}

void request_begin(const std::string &handle, std::function<void()> close) {
  fld::glib_call([handle, close] {
    static const GDBusInterfaceVTable vt = {request_call, nullptr, nullptr, {}};
    GError *err = nullptr;
    guint id = g_dbus_connection_register_object(g_conn, handle.c_str(), g_request_info->interfaces[0], &vt, nullptr,
                                                 nullptr, &err);
    if (!id) {
      g_printerr("xdg-desktop-portal-fl: cannot export %s: %s\n", handle.c_str(), err ? err->message : "?");
      g_clear_error(&err);
    }
    std::lock_guard<std::mutex> l(g_req_mutex);
    g_requests[handle] = {id, close};
  });
}

void request_end(const std::string &handle) {
  fld::glib_call([handle] {
    std::lock_guard<std::mutex> l(g_req_mutex);
    auto it = g_requests.find(handle);
    if (it == g_requests.end()) return;
    if (it->second.reg) g_dbus_connection_unregister_object(g_conn, it->second.reg);
    g_requests.erase(it);
  });
}

static void start_bus() {
  fld::glib_call([] {
    GError *err = nullptr;
    g_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!g_conn) {
      fprintf(stderr, "xdg-desktop-portal-fl: no session bus: %s\n", err ? err->message : "?");
      exit(1);
    }
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(kXml, &err);
    if (!info) {
      fprintf(stderr, "xdg-desktop-portal-fl: %s\n", err->message);
      exit(1);
    }
    g_request_info = g_dbus_node_info_new_for_xml(kRequestXml, nullptr);
    static const GDBusInterfaceVTable vt = {method_call, get_property, nullptr, {}};
    for (int i = 0; info->interfaces[i]; i++)
      g_dbus_connection_register_object(g_conn, kPath, info->interfaces[i], &vt, nullptr, nullptr, nullptr);
    GBusNameOwnerFlags flags = G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
    if (g_replace) flags = (GBusNameOwnerFlags)(flags | G_BUS_NAME_OWNER_FLAGS_REPLACE);
    g_bus_own_name_on_connection(
        g_conn, "org.freedesktop.impl.portal.desktop.fl", flags, nullptr,
        [](GDBusConnection *, const char *, gpointer) {
          fprintf(stderr, "xdg-desktop-portal-fl: lost the bus name, exiting\n");
          exit(0);
        },
        nullptr, nullptr);
  });
}

} // namespace portal

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-r" || a == "--replace") portal::g_replace = true;
    else if (a == "-h" || a == "--help") {
      printf("Usage: xdg-desktop-portal-fl [--replace]\n"
             "fldesktop backend of xdg-desktop-portal (normally started by D-Bus).\n");
      return 0;
    }
  }
  // Own the bus name before FLTK opens the display: libdecor reads the cursor
  // and colour scheme from org.freedesktop.portal.Desktop with a blocking
  // call, and xdg-desktop-portal answers it only once it reached its backends
  // (us). Asked for afterwards, the name came 25 s late (the D-Bus timeout)
  // and xdg-desktop-portal had already fallen back to other backends.
  Fl::lock();
  fld::glib_start();
  portal::start_bus();
  fld::init_app("xdg-desktop-portal-fl", argc, argv);
  portal::settings_watch();
  while (true) Fl::wait(1e20);
  return 0;
}
