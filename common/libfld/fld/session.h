// Session and power actions shared by the panel's power menu and the
// launcher's power buttons.
#pragma once

namespace fld {

enum class PowerAction { Lock, LogOut, Suspend, Reboot, PowerOff };

bool can_lock();     // fl-lock or swaylock is installed
void lock_screen();  // fl-lock (asks fl-wm to lock), else swaylock
void log_out();      // ends fl-wm ($FLWM_PID), else exits this program
void suspend();
void reboot();
void power_off();

// Asks first for log out, restart and shut down (Lock and Suspend just run);
// returns true when the action was started.
bool confirm_power(PowerAction a);

const char *power_label(PowerAction a); // "Lock Screen", "Log Out", ...
const char *power_icon(PowerAction a);  // icon theme name

} // namespace fld
