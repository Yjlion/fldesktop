// fl-lock --daemon: locks the session when logind asks (loginctl lock-session,
// lid close handled by logind) and before the system suspends.
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"

namespace {

GDBusConnection *g_bus;
GMainLoop *g_loop;
int g_inhibit_fd = -1;

bool lock_on_suspend() {
  fld::Ini ini;
  ini.load(fld::desktop_config_path());
  return ini.get_bool("Lock", "OnSuspend", true);
}

void request_lock() {
  // the same as running "fl-lock": a signal to fl-wm
  fld::spawn({"fl-lock"});
}

// a "delay" inhibitor lets us lock before the machine sleeps
void take_inhibitor() {
  if (g_inhibit_fd >= 0 || !lock_on_suspend()) return;
  GUnixFDList *fds = nullptr;
  GError *err = nullptr;
  GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(
      g_bus, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit",
      g_variant_new("(ssss)", "sleep", "fldesktop", "Lock the screen before suspending", "delay"),
      G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &fds, nullptr, &err);
  if (!r) {
    fprintf(stderr, "fl-lock: cannot take a sleep inhibitor: %s\n", err->message);
    g_error_free(err);
    return;
  }
  gint32 idx;
  g_variant_get(r, "(h)", &idx);
  g_inhibit_fd = g_unix_fd_list_get(fds, idx, nullptr);
  g_variant_unref(r);
  g_object_unref(fds);
}

void release_inhibitor() {
  if (g_inhibit_fd >= 0) close(g_inhibit_fd);
  g_inhibit_fd = -1;
}

gboolean release_later(gpointer) {
  release_inhibitor();
  return G_SOURCE_REMOVE;
}

void on_signal(GDBusConnection *, const char *, const char *, const char *iface, const char *signal,
               GVariant *params, gpointer) {
  std::string s = signal;
  if (s == "Lock") {
    request_lock();
  } else if (s == "PrepareForSleep") {
    gboolean start;
    g_variant_get(params, "(b)", &start);
    if (start) {
      if (lock_on_suspend()) request_lock();
      // give the locker a moment to cover the screen, then let the system sleep
      g_timeout_add(600, release_later, nullptr);
    } else {
      take_inhibitor();
    }
  }
  (void)iface;
}

std::string session_path() {
  GError *err = nullptr;
  const char *id = getenv("XDG_SESSION_ID");
  GVariant *r = nullptr;
  if (id && *id)
    r = g_dbus_connection_call_sync(g_bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                    "org.freedesktop.login1.Manager", "GetSession", g_variant_new("(s)", id),
                                    G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
  if (!r) {
    g_clear_error(&err);
    r = g_dbus_connection_call_sync(g_bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                    "org.freedesktop.login1.Manager", "GetSessionByPID",
                                    g_variant_new("(u)", (guint32)getpid()), G_VARIANT_TYPE("(o)"),
                                    G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
  }
  if (!r) {
    fprintf(stderr, "fl-lock: no logind session: %s\n", err ? err->message : "?");
    g_clear_error(&err);
    return "";
  }
  const char *path;
  g_variant_get(r, "(&o)", &path);
  std::string p = path;
  g_variant_unref(r);
  return p;
}

}  // namespace

int run_daemon() {
  GError *err = nullptr;
  g_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
  if (!g_bus) {
    fprintf(stderr, "fl-lock: no system bus: %s\n", err->message);
    return 1;
  }
  std::string path = session_path();
  if (!path.empty())
    g_dbus_connection_signal_subscribe(g_bus, "org.freedesktop.login1", "org.freedesktop.login1.Session", "Lock",
                                       path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_signal, nullptr, nullptr);
  g_dbus_connection_signal_subscribe(g_bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
                                     "PrepareForSleep", "/org/freedesktop/login1", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                     on_signal, nullptr, nullptr);
  take_inhibitor();
  g_loop = g_main_loop_new(nullptr, FALSE);
  g_main_loop_run(g_loop);
  return 0;
}
