#include "fld/session.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <atomic>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <memory>
#include <string>
#include <unistd.h>
#include <signal.h>
#include <mutex>
#include <thread>

#ifdef FLD_HAVE_GIO
#include <gio/gio.h>
#endif

namespace fld {

static std::string locker() {
  if (!which("fl-lock").empty()) return "fl-lock";
  if (!which("swaylock").empty()) return "swaylock";
  return "";
}

bool can_lock() { return !locker().empty(); }

void lock_screen() {
  std::string l = locker();
  if (l == "fl-lock") spawn({"fl-lock"});
  else if (l == "swaylock") spawn({"swaylock", "-f", "-c", "1e1f22"});
}

void log_out() {
  const char *pid = getenv("FLWM_PID");
  if (pid && atoi(pid) > 1) kill(atoi(pid), SIGTERM);
  else exit(0);
}

std::vector<std::string> power_command(const std::string &action, bool have_loginctl, bool have_systemctl) {
  // systemctl on systemd systems: systemd's loginctl has no power verbs (only
  // elogind's has), so loginctl is only the fallback for elogind (Devuan, Alpine)
  if (have_systemctl) return {"systemctl", action};
  if (have_loginctl) return {"loginctl", action};
  return {};
}

namespace {

std::atomic<int> g_power_pending{0};

const char *power_verb(const std::string &action) {
  if (action == "poweroff") return "shut down";
  if (action == "reboot") return "restart";
  return action == "hibernate" ? "hibernate" : "suspend";
}

struct PowerFailure {
  std::string action, message;
};

void show_power_failure(void *d) {
  std::unique_ptr<PowerFailure> f(static_cast<PowerFailure *>(d));
  fl_alert("Could not %s the computer.\n\n%s", power_verb(f->action), f->message.c_str());
}

#ifdef FLD_HAVE_GIO
const char *logind_method(const std::string &action) {
  if (action == "reboot") return "Reboot";
  if (action == "poweroff") return "PowerOff";
  if (action == "suspend") return "Suspend";
  if (action == "hibernate") return "Hibernate";
  return nullptr;
}

bool logind_missing(const GError *e) {
  return g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN) ||
         g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_NAME_HAS_NO_OWNER) ||
         g_error_matches(e, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD) ||
         g_error_matches(e, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
}

// A call to logind's Manager on the system bus. 0: done, 1: logind is not
// there (no system bus, no logind), 2: refused or failed (message in err).
int logind_call(const char *method, GVariant *args, const GVariantType *reply_type, int timeout_ms,
                GVariant **reply, std::string &err) {
  GError *e = nullptr;
  // one connection for the program: each new one costs a handshake with the
  // bus (three of them made the launcher wait before it showed)
  static std::mutex bus_lock;
  static GDBusConnection *cached = nullptr;
  GDBusConnection *bus;
  {
    std::lock_guard<std::mutex> l(bus_lock);
    if (cached && g_dbus_connection_is_closed(cached)) {
      g_object_unref(cached);
      cached = nullptr;
    }
    if (!cached) cached = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e);
    bus = cached ? (GDBusConnection *)g_object_ref(cached) : nullptr;
  }
  if (!bus) {
    if (args) g_variant_unref(g_variant_ref_sink(args));
    if (e) g_error_free(e);
    return 1;
  }
  GVariant *r = g_dbus_connection_call_sync(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                                            "org.freedesktop.login1.Manager", method, args, reply_type,
                                            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, timeout_ms,
                                            nullptr, &e);
  g_object_unref(bus);
  if (r) {
    if (reply) *reply = r;
    else g_variant_unref(r);
    return 0;
  }
  int rc = logind_missing(e) ? 1 : 2;
  if (rc == 2) {
    g_dbus_error_strip_remote_error(e);
    err = e->message;
  }
  g_error_free(e);
  return rc;
}
#endif

// Runs the action: logind over D-Bus (systemd-logind and elogind both have it;
// polkit asks through fl-polkit-agent when needed), else systemctl / loginctl.
// Returns the reason it failed, or "" when it was started.
std::string do_power(const std::string &action) {
  std::string err;
#ifdef FLD_HAVE_GIO
  if (const char *m = logind_method(action)) {
    // interactive: polkit may ask for a password, which can take a while
    int rc = logind_call(m, g_variant_new("(b)", TRUE), nullptr, G_MAXINT, nullptr, err);
    if (rc != 1) return err;
  }
#endif
  auto argv = power_command(action, !which("loginctl").empty(), !which("systemctl").empty());
  if (argv.empty()) return "Neither logind (systemd-logind or elogind) nor systemctl is available.";
  std::string out;
  int st = run_capture_input(argv, "", out, true);
  if (st == 0) return "";
  err = trim(out);
  if (err.empty()) err = join(argv, " ") + " failed (exit status " + std::to_string(st) + ").";
  return err;
}

} // namespace

bool run_power(const std::string &action) {
  // Fl::awake() from the worker needs FLTK's thread support switched on
  Fl::lock();
  Fl::unlock();
  g_power_pending++;
  std::thread([action] {
    std::string err = do_power(action);
    if (!err.empty()) Fl::awake(show_power_failure, new PowerFailure{action, err});
    g_power_pending--;
    Fl::awake();
  }).detach();
  return true;
}

bool power_pending() { return g_power_pending > 0; }

void wait_power() {
  while (power_pending()) Fl::wait(0.2);
  Fl::flush();
  // a failure dialog queued just before the counter dropped
  Fl::wait(0);
}

bool can_power(PowerAction a) {
  if (a == PowerAction::Lock) return can_lock();
  if (a == PowerAction::LogOut) return true;
#ifdef FLD_HAVE_GIO
  const char *m = a == PowerAction::Suspend ? "CanSuspend" : a == PowerAction::Reboot ? "CanReboot" : "CanPowerOff";
  GVariant *r = nullptr;
  std::string err;
  if (logind_call(m, nullptr, G_VARIANT_TYPE("(s)"), 2000, &r, err) == 0) {
    const char *v = nullptr;
    g_variant_get(r, "(&s)", &v);
    // "yes", "challenge" (polkit asks) or "no", "na" (not possible here)
    bool ok = v && strcmp(v, "no") != 0 && strcmp(v, "na") != 0;
    g_variant_unref(r);
    return ok;
  }
#endif
  return true;
}

void suspend() { run_power("suspend"); }

std::string launcher_pidfile() {
  return getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-launcher-" + std::to_string(getuid()) + ".pid";
}

std::string launcher_closed_stamp() {
  return getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-launcher-" + std::to_string(getuid()) + ".closed";
}

int resident_launcher() {
  std::string s;
  if (!read_file(launcher_pidfile(), s) || s.find("daemon") == std::string::npos) return 0;
  int pid = atoi(s.c_str());
  if (pid <= 1) return 0;
  // make sure it really is a launcher (the pid may have been reused)
  std::string comm;
  read_file("/proc/" + std::to_string(pid) + "/comm", comm);
  return trim(comm) == "fl-launcher" ? pid : 0;
}

void toggle_launcher() {
  int pid = resident_launcher();
  if (pid && kill(pid, SIGUSR1) == 0) return;
  spawn({"fl-launcher"});
}
void reboot() { run_power("reboot"); }
void power_off() { run_power("poweroff"); }

const char *power_label(PowerAction a) {
  switch (a) {
    case PowerAction::Lock: return "Lock Screen";
    case PowerAction::LogOut: return "Log Out";
    case PowerAction::Suspend: return "Suspend";
    case PowerAction::Reboot: return "Restart";
    case PowerAction::PowerOff: return "Shut Down";
  }
  return "";
}

const char *power_icon(PowerAction a) {
  switch (a) {
    case PowerAction::Lock: return "system-lock-screen";
    case PowerAction::LogOut: return "system-log-out";
    case PowerAction::Suspend: return "system-suspend";
    case PowerAction::Reboot: return "system-reboot";
    case PowerAction::PowerOff: return "system-shutdown";
  }
  return "";
}

bool confirm_power(PowerAction a) {
  switch (a) {
    case PowerAction::Lock:
      if (!can_lock()) return false;
      lock_screen();
      return true;
    case PowerAction::Suspend:
      suspend();
      return true;
    case PowerAction::LogOut:
      if (fl_choice("Log out of this session?", "Cancel", "Log Out", nullptr) != 1) return false;
      log_out();
      return true;
    case PowerAction::Reboot:
      if (fl_choice("Restart the computer?", "Cancel", "Restart", nullptr) != 1) return false;
      reboot();
      return true;
    case PowerAction::PowerOff:
      if (fl_choice("Shut down the computer?", "Cancel", "Shut Down", nullptr) != 1) return false;
      power_off();
      return true;
  }
  return false;
}

} // namespace fld
