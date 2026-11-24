// Session and power actions shared by the panel's power menu and the
// launcher's power buttons.
#pragma once

#include <string>
#include <vector>

namespace fld {

enum class PowerAction { Lock, LogOut, Suspend, Reboot, PowerOff };

bool can_lock();     // fl-lock or swaylock is installed
void lock_screen();  // fl-lock (asks fl-wm to lock), else swaylock
void log_out();      // ends fl-wm ($FLWM_PID), else exits this program
void suspend();
void reboot();
void power_off();

// "suspend", "hibernate", "reboot" or "poweroff": asks logind over D-Bus
// (systemd-logind or elogind; polkit may ask for a password), else runs
// systemctl or elogind's loginctl. Works in a thread: returns at once, and
// shows a dialog if the action is refused or fails.
bool run_power(const std::string &action);
// true while a run_power() request has not finished
bool power_pending();
// runs the event loop until requests have finished (a program that is about
// to exit calls this so their error dialogs are still shown)
void wait_power();
// false when logind says the action is not possible here ("no", "na")
bool can_power(PowerAction a);
// the command run_power() falls back to without logind on the system bus:
// systemctl, else loginctl (elogind; systemd's loginctl has no power verbs)
std::vector<std::string> power_command(const std::string &action, bool have_loginctl, bool have_systemctl);

// Asks first for log out, restart and shut down (Lock and Suspend just run);
// returns true when the action was started.
bool confirm_power(PowerAction a);

const char *power_label(PowerAction a); // "Lock Screen", "Log Out", ...
const char *power_icon(PowerAction a);  // icon theme name

} // namespace fld
