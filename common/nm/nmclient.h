// NetworkManager access for fldesktop (libnm on the fld-glib thread).
//
// The UI only sees plain snapshots (State); every action runs on the GLib
// thread and reports back in the FLTK thread. Call Fl::lock() once in main().
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "vpn_util.h"

namespace nm {

struct AccessPoint {
  std::string ssid, bssid, path;
  int strength = 0;        // 0..100
  bool secure = false;
  std::string security;    // "", "WPA2", "WPA3", "WPA2/3", "WEP", "802.1X"
  bool enterprise = false;
  bool sae = false;        // WPA3 personal
  bool active = false;
  uint32_t frequency = 0;  // MHz
};

struct Device {
  std::string path, iface, type; // type: ethernet, wifi, bridge, bond, vlan, loopback, other
  std::string type_description, driver, hwaddr, state;
  int state_code = 0;            // NMDeviceState
  bool managed = true, available = true, connected = false, connecting = false;
  std::string active_id, active_uuid;
  std::vector<std::string> ip4, ip6; // "192.168.1.5/24"
  std::string gateway4, gateway6;
  std::vector<std::string> dns;
  uint32_t speed = 0;            // Mb/s
  std::vector<AccessPoint> aps;  // Wi-Fi only, strongest first, one per SSID
};

struct Connection {
  std::string path, id, uuid, type, iface; // type: 802-3-ethernet, 802-11-wireless, vpn, wireguard, ...
  std::string ssid;                        // Wi-Fi
  bool active = false, autoconnect = true;
  long long timestamp = 0;                 // last used
};

struct State {
  bool running = false;          // NetworkManager is available
  bool networking = true, wireless = true, wireless_hw = true;
  std::string connectivity;      // full, limited, portal, none, unknown
  std::string primary_id, primary_type;
  int primary_strength = -1;     // Wi-Fi strength of the primary connection
  std::vector<Device> devices;
  std::vector<Connection> connections;
  const Device *device(const std::string &path) const;
  const Connection *connection(const std::string &uuid) const;
};

// Editable settings of one connection (what the editor shows).
struct Settings {
  std::string uuid, id, type, iface;
  bool autoconnect = true, all_users = true;
  // Wi-Fi
  std::string ssid, security = "none"; // none, wpa-psk, sae
  std::string psk;                     // empty = keep the stored one
  bool hidden = false;
  // IP
  std::string ip4_method = "auto", ip6_method = "auto"; // auto, manual, disabled, link-local, shared / ignore
  std::string ip4_addresses, ip4_gateway, ip4_dns;      // addresses: "a.b.c.d/prefix" separated by spaces/newlines
  std::string ip6_addresses, ip6_gateway, ip6_dns;
  bool ip4_ignore_auto_dns = false, ip6_ignore_auto_dns = false;
  // WireGuard (type "wireguard")
  std::string wg_private_key; // the stored key when it could be read; "" = keep the stored one
  bool wg_has_private_key = false;
  int wg_listen_port = 0, wg_mtu = 0;
  std::vector<WgPeer> wg_peers;
  // VPN plugins (type "vpn"): OpenVPN, OpenConnect, ...
  std::string vpn_service, vpn_user;
  std::string vpn_password; // "" = keep the stored one
  std::vector<std::pair<std::string, std::string>> vpn_data;
};

// An installed NetworkManager VPN plugin.
struct VpnPlugin {
  std::string service, name;
};
std::vector<VpnPlugin> vpn_plugins();

using Done = std::function<void(const std::string &error)>; // "" = success

class Client {
public:
  static Client &get();
  // Starts the GLib thread (if needed) and connects to NetworkManager.
  void start();
  const State &state() const { return state_; }
  std::function<void()> on_changed; // FLTK thread

  void set_networking(bool on, Done done = nullptr);
  void set_wireless(bool on, Done done = nullptr);
  void scan(const std::string &device_path);
  // an empty uuid lets NetworkManager pick the connection for device_path
  void activate(const std::string &uuid, const std::string &device_path, Done done = nullptr);
  void deactivate(const std::string &uuid, Done done = nullptr);
  void disconnect_device(const std::string &device_path, Done done = nullptr);
  // Connects to a Wi-Fi network: uses a saved connection for the SSID or creates one.
  void connect_wifi(const std::string &device_path, const std::string &ssid, const std::string &password,
                    bool hidden, Done done = nullptr);
  void delete_connection(const std::string &uuid, Done done = nullptr);
  void get_settings(const std::string &uuid, std::function<void(const Settings &, const std::string &err)> cb);
  // Saves (uuid set) or adds (uuid empty) a connection.
  void save_settings(const Settings &s, Done done = nullptr);
  // Imports a VPN configuration file: a WireGuard wg-quick file, or whatever
  // an installed VPN plugin understands (.ovpn, ...). done(err, uuid).
  void import_vpn(const std::string &path, std::function<void(const std::string &err, const std::string &uuid)> done);
  // true if a saved Wi-Fi connection exists for the SSID
  bool has_saved_wifi(const std::string &ssid) const;

  // Hosts a secret agent that asks for Wi-Fi and VPN passwords. `ask` runs in
  // the FLTK thread with the network's name (VPNs: "VPN <name>") and must
  // call reply(password) or reply("") to cancel.
  void start_secret_agent(std::function<void(const std::string &ssid, std::function<void(const std::string &)> reply)> ask);

  State state_; // internal: written in the FLTK thread only
private:
  Client() = default;
};

// helpers (also used by the tests)
bool parse_address(const std::string &text, std::string &addr, unsigned &prefix, bool ipv6);
std::vector<std::string> split_list(const std::string &text); // spaces, commas, newlines
const char *signal_icon(int strength); // network-wireless-signal-*

} // namespace nm
