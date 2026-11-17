// BlueZ access for fldesktop (GDBus on the fld-glib thread).
//
// The UI sees plain snapshots (State); actions run on the GLib thread and
// report back in the FLTK thread. Call Fl::lock() once in main().
// FLD_BLUEZ_BUS=session makes the client use the session bus (tests).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace bt {

struct Adapter {
  std::string path, address, name, alias;
  bool powered = false, discoverable = false, pairable = false, discovering = false;
};

struct Device {
  std::string path, adapter, address, name, alias, icon;
  bool paired = false, trusted = false, connected = false, blocked = false, bonded = false;
  int16_t rssi = 0;
  bool has_rssi = false;
  int battery = -1;            // percent, -1 unknown
  uint32_t device_class = 0;
  bool busy = false;           // a pair/connect call is running (UI side)
  std::string display_name() const { return !alias.empty() ? alias : !name.empty() ? name : address; }
};

struct State {
  bool running = false;        // bluetoothd is on the bus
  bool rfkill_blocked = false; // soft or hard blocked
  bool rfkill_hard = false;
  std::vector<Adapter> adapters;
  std::vector<Device> devices; // paired first, then by name
  const Adapter *adapter() const { return adapters.empty() ? nullptr : &adapters[0]; }
  const Device *device(const std::string &path) const;
  int connected_count() const;
};

// One question from the pairing agent.
struct AgentRequest {
  enum Kind { PIN, PASSKEY, DISPLAY_PIN, DISPLAY_PASSKEY, CONFIRM, AUTHORIZE, AUTHORIZE_SERVICE } kind;
  std::string device_path, device_name, value; // value: PIN / passkey to show or confirm, service UUID
};

using Done = std::function<void(const std::string &error)>; // "" = success

class Client {
public:
  static Client &get();
  void start();
  const State &state() const { return state_; }
  std::function<void()> on_changed; // FLTK thread
  // FLTK thread: a device connected (true) or disconnected
  std::function<void(const Device &, bool connected)> on_connection;

  void set_powered(bool on, Done done = nullptr);
  void set_discoverable(bool on, Done done = nullptr);
  void set_alias(const std::string &alias, Done done = nullptr);
  // discovery runs while at least one view wants it
  void discovery(bool on);
  void pair(const std::string &device, Done done = nullptr);
  void connect(const std::string &device, Done done = nullptr);
  void disconnect(const std::string &device, Done done = nullptr);
  void set_trusted(const std::string &device, bool on, Done done = nullptr);
  void set_blocked(const std::string &device, bool on, Done done = nullptr);
  void remove(const std::string &device, Done done = nullptr);

  // Registers a pairing agent. `ask` runs in the FLTK thread and must call
  // reply(ok, value) once (value: PIN text or passkey digits).
  void start_agent(std::function<void(const AgentRequest &, std::function<void(bool, const std::string &)> reply)> ask);

  State state_; // internal: written in the FLTK thread only
private:
  Client() = default;
};

// helpers (unit tested)
std::string error_text(const std::string &dbus_error_name, const std::string &message);
const char *device_icon(const std::string &bluez_icon, uint32_t device_class);
std::string format_passkey(uint32_t passkey); // 6 digits, zero padded
// reads /sys/class/rfkill: any bluetooth switch soft / hard blocked
void rfkill_state(const std::string &sysfs_root, bool &soft, bool &hard);
bool rfkill_unblock(); // writes /dev/rfkill

} // namespace bt
