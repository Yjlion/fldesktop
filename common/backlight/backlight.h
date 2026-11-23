// Screen and keyboard backlights for fldesktop.
//
// The screens are /sys/class/backlight/*; their brightness is set through
// logind (Session.SetBrightness, no root needed), else by writing the sysfs
// file when it is writable, else through fl-backlight-helper with pkexec.
// The keyboard backlight goes through UPower's KbdBacklight.
//
// FLD_SYSFS_ROOT puts /sys somewhere else and FLD_LOGIND_BUS=session /
// FLD_UPOWER_BUS=session use the session bus (tests).
#pragma once
#include <string>
#include <vector>

namespace bl {

struct Device {
  std::string name;  // intel_backlight, acpi_video0, ...
  std::string type;  // firmware, platform, raw
  int brightness = 0, max = 0;
};

// The sysfs root ("" or $FLD_SYSFS_ROOT) and the backlight devices under it.
std::string sysfs_root();
std::vector<Device> list();
// The device to use: firmware before platform before raw (as systemd does),
// then the one with the most steps; null for none.
const Device *pick(const std::vector<Device> &devs);
// Reads the device's brightness again.
bool refresh(Device &d);

// Percent of a raw value and back. A screen is never set fully dark by a
// percentage (1 % is at least 1 raw step).
int percent(int raw, int max);
int raw_for_percent(int pct, int max);
// The raw value one step up (dir > 0) or down from raw, in steps of `step`
// percent, and never below 1 %.
int step(int raw, int max, int dir, int step_pct = 5);

// Sets a screen's brightness (raw). err says why it failed.
bool set(const Device &d, int raw, std::string &err);

// The screen as a whole: false when there is no backlight.
bool screen_get(int &pct);
bool screen_set(int pct, std::string &err);
// One step up or down; pct gets the new percentage.
bool screen_step(int dir, int &pct, std::string &err);

// The keyboard backlight (UPower): false without one.
bool kbd_get(int &value, int &max);
bool kbd_set(int value);
// One step up or down, wrapping from the top to off when `wrap` (the one
// "keyboard light" key some laptops have); pct gets the new percentage.
bool kbd_step(int dir, bool wrap, int &pct);

} // namespace bl
