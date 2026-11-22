// fl-lock --daemon: locks the session when logind asks (loginctl lock-session,
// lid close handled by logind) and before the system suspends.
//
// One daemon per session: it owns org.fldesktop.LockDaemon on the session bus
// (a second one exits), and it ends with its session: when the fl-wm that
// started it ($FLWM_PID) exits or logind removes the session. It has no
// Wayland connection that would end it otherwise, and user processes may
// outlive a logout.
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glib-unix.h>

#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"

namespace {

GDBusConnection *g_bus;
GMainLoop *g_loop;
int g_inhibit_fd = -1;
std::string g_session; // our logind session's object path

void quit(const char *why) {
  fprintf(stderr, "fl-lock: daemon exits: %s\n", why);
  if (g_loop) g_main_loop_quit(g_loop);
}

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
  } else if (s == "SessionRemoved") {
    const char *id, *path;
    g_variant_get(params, "(&s&o)", &id, &path);
    if (!g_session.empty() && g_session == path) quit("the session ended");
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

// the fl-wm that started us ($FLWM_PID), when it is one
pid_t compositor_pid() {
  const char *e = getenv("FLWM_PID");
  pid_t pid = e ? (pid_t)atoi(e) : 0;
  if (pid <= 0) return 0;
  std::string comm;
  if (!fld::read_file("/proc/" + std::to_string(pid) + "/comm", comm) || fld::trim(comm) != "fl-wm") return 0;
  return pid;
}

gboolean compositor_gone(gint, GIOCondition, gpointer) {
  quit("the compositor exited");
  return G_SOURCE_REMOVE;
}

gboolean compositor_check(gpointer d) {
  if (kill((pid_t)GPOINTER_TO_INT(d), 0) == 0 || errno == EPERM) return G_SOURCE_CONTINUE;
  quit("the compositor exited");
  return G_SOURCE_REMOVE;
}

void watch_compositor() {
  pid_t pid = compositor_pid();
  if (!pid) return;
#ifdef SYS_pidfd_open
  // readable when the process exits: no wakeups until then
  int fd = (int)syscall(SYS_pidfd_open, pid, 0);
  if (fd >= 0) {
    g_unix_fd_add(fd, G_IO_IN, compositor_gone, nullptr);
    return;
  }
#endif
  g_timeout_add_seconds(5, compositor_check, GINT_TO_POINTER((int)pid));
}

void start() {
  g_session = session_path();
  if (!g_session.empty()) {
    g_dbus_connection_signal_subscribe(g_bus, "org.freedesktop.login1", "org.freedesktop.login1.Session", "Lock",
                                       g_session.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_signal, nullptr,
                                       nullptr);
    g_dbus_connection_signal_subscribe(g_bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
                                       "SessionRemoved", "/org/freedesktop/login1", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_signal, nullptr, nullptr);
  }
  g_dbus_connection_signal_subscribe(g_bus, "org.freedesktop.login1", "org.freedesktop.login1.Manager",
                                     "PrepareForSleep", "/org/freedesktop/login1", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                     on_signal, nullptr, nullptr);
  take_inhibitor();
}

}  // namespace

int run_daemon() {
  GError *err = nullptr;
  g_bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
  if (!g_bus) {
    fprintf(stderr, "fl-lock: no system bus: %s\n", err->message);
    return 1;
  }
  g_loop = g_main_loop_new(nullptr, FALSE);
  watch_compositor();
  // one per session: the first to own the name runs, the others leave
  GDBusConnection *session = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
  if (session) {
    g_bus_own_name_on_connection(
        session, "org.fldesktop.LockDaemon", G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
        [](GDBusConnection *, const char *, gpointer) { start(); },
        [](GDBusConnection *c, const char *, gpointer) {
          quit(c && !g_dbus_connection_is_closed(c) ? "another one runs in this session" : "the session bus closed");
        },
        nullptr, nullptr);
  } else {
    fprintf(stderr, "fl-lock: no session bus (%s): cannot tell whether another daemon runs\n", err->message);
    g_clear_error(&err);
    start();
  }
  g_main_loop_run(g_loop);
  release_inhibitor();
  return 0;
}
