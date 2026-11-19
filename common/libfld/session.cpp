#include "fld/session.h"
#include "fld/util.h"

#include <FL/fl_ask.H>
#include <csignal>
#include <cstdlib>
#include <string>

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

void suspend() { spawn({"systemctl", "suspend"}); }
void reboot() { spawn({"systemctl", "reboot"}); }
void power_off() { spawn({"systemctl", "poweroff"}); }

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
