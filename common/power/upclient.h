// UPower and power-profiles-daemon access for fldesktop (GDBus on the
// fld-glib thread): battery state, power devices, power profiles.
//
// The UI sees plain snapshots (State); call Fl::lock() once in main().
// FLD_UPOWER_BUS=session makes the client use the session bus (tests).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace up {

// UPower device states (org.freedesktop.UPower.Device.State)
enum BatteryState { UNKNOWN = 0, CHARGING = 1, DISCHARGING = 2, EMPTY = 3, FULL = 4, PENDING_CHARGE = 5, PENDING_DISCHARGE = 6 };

struct Device {
  std::string path, model, vendor, native_path, icon;
  int type = 0;                 // UPower device type: 1 line power, 2 battery, 3 UPS, 5 mouse, 6 keyboard, ...
  bool power_supply = false;    // powers the computer
  bool present = false;
  double percentage = -1;       // -1 unknown
  int state = UNKNOWN;
  long long time_to_empty = 0, time_to_full = 0; // seconds, 0 unknown
  double energy = 0, energy_full = 0, energy_full_design = 0, energy_rate = 0, voltage = 0; // Wh, W, V
  double capacity = 0;          // health in percent
  std::string technology;       // Li-ion, ...
  std::string name() const;     // "Battery", "Logitech MX Master", ...
};

struct State {
  bool running = false;         // upowerd is on the bus
  bool on_battery = false;
  bool lid_present = false, lid_closed = false;
  bool has_battery = false;     // the display device is a battery
  Device display;               // the composite battery ("DisplayDevice")
  std::vector<Device> devices;  // batteries and peripherals (no line power)
  // power-profiles-daemon
  bool profiles_available = false;
  std::string profile;          // power-saver, balanced, performance
  std::vector<std::string> profiles;
  std::string degraded;         // why performance is degraded ("" when not)
};

class Client {
public:
  static Client &get();
  void start();
  const State &state() const { return state_; }
  std::function<void()> on_changed; // FLTK thread

  void set_profile(const std::string &profile, std::function<void(const std::string &error)> done = nullptr);

  State state_; // internal: written in the FLTK thread only
private:
  Client() = default;
};

// helpers (unit tested)
std::string icon_for(double percentage, int state, bool on_battery); // battery-good-charging, ...
std::string format_time(long long seconds);                          // "2 h 05 min", "35 min"
std::string summary(const State &s);                                 // "85 %, 2 h 05 min left"
std::string profile_label(const std::string &profile);               // "Power Saver", ...

// Low battery handling: the level for a percentage (and what was announced),
// so the panel warns once per level while discharging.
enum Level { LEVEL_OK, LEVEL_LOW, LEVEL_CRITICAL, LEVEL_ACTION };
Level battery_level(double percentage, bool discharging, int low = 10, int critical = 5, int action = 3);

} // namespace up
