#include "nmclient.h"

#include <NetworkManager.h>
#include <nm-secret-agent-old.h>

// Some libnm versions declare nm_secret_agent_error_quark() without C linkage
// (nm-errors.h has no G_BEGIN_DECLS), so look the domain up by its name.
static GQuark secret_agent_error() { return g_quark_from_static_string("nm-secret-agent-error-quark"); }
#include <arpa/inet.h>

#include <algorithm>
#include <cstring>
#include <strings.h>
#include <map>
#include <memory>
#include <set>

#include "fld/glib_bridge.h"

namespace nm {

// ---------------------------------------------------------------------------
// helpers

const Device *State::device(const std::string &path) const {
  for (auto &d : devices) if (d.path == path) return &d;
  return nullptr;
}

const Connection *State::connection(const std::string &uuid) const {
  for (auto &c : connections) if (c.uuid == uuid) return &c;
  return nullptr;
}

std::vector<std::string> split_list(const std::string &text) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : text) {
    if (c == ' ' || c == ',' || c == ';' || c == '\n' || c == '\t' || c == '\r') {
      if (!cur.empty()) v.push_back(cur);
      cur.clear();
    } else cur += c;
  }
  if (!cur.empty()) v.push_back(cur);
  return v;
}

bool parse_address(const std::string &text, std::string &addr, unsigned &prefix, bool ipv6) {
  size_t slash = text.find('/');
  addr = text.substr(0, slash);
  prefix = ipv6 ? 64 : 24;
  if (slash != std::string::npos) {
    std::string p = text.substr(slash + 1);
    if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos) return false;
    prefix = (unsigned)atoi(p.c_str());
    if (prefix > (ipv6 ? 128u : 32u)) return false;
  }
  unsigned char buf[16];
  return inet_pton(ipv6 ? AF_INET6 : AF_INET, addr.c_str(), buf) == 1;
}

const char *signal_icon(int s) {
  if (s < 0) return "network-wireless-signal-none";
  if (s >= 75) return "network-wireless-signal-excellent";
  if (s >= 50) return "network-wireless-signal-good";
  if (s >= 25) return "network-wireless-signal-ok";
  if (s > 0) return "network-wireless-signal-weak";
  return "network-wireless-signal-none";
}

// ---------------------------------------------------------------------------
// GLib thread state

namespace {

NMClient *g_client = nullptr;
guint g_refresh_source = 0;
std::set<gpointer> g_watched; // objects with our signal handlers

void schedule_refresh();

std::string str(const char *s) { return s ? s : ""; }

std::string ssid_of(GBytes *b) {
  if (!b) return "";
  gsize len;
  const guint8 *data = (const guint8 *)g_bytes_get_data(b, &len);
  char *s = nm_utils_ssid_to_utf8(data, len);
  std::string r = s ? s : "";
  g_free(s);
  return r;
}

const char *device_type(NMDevice *d) {
  switch (nm_device_get_device_type(d)) {
    case NM_DEVICE_TYPE_ETHERNET:
    case NM_DEVICE_TYPE_VETH: return "ethernet";
    case NM_DEVICE_TYPE_WIFI: return "wifi";
    case NM_DEVICE_TYPE_BRIDGE: return "bridge";
    case NM_DEVICE_TYPE_BOND: return "bond";
    case NM_DEVICE_TYPE_VLAN: return "vlan";
    case NM_DEVICE_TYPE_LOOPBACK: return "loopback";
    case NM_DEVICE_TYPE_WIREGUARD: return "wireguard";
    case NM_DEVICE_TYPE_MODEM: return "modem";
    default: return "other";
  }
}

const char *state_text(NMDeviceState s) {
  switch (s) {
    case NM_DEVICE_STATE_UNMANAGED: return "Unmanaged";
    case NM_DEVICE_STATE_UNAVAILABLE: return "Unavailable";
    case NM_DEVICE_STATE_DISCONNECTED: return "Disconnected";
    case NM_DEVICE_STATE_PREPARE:
    case NM_DEVICE_STATE_CONFIG: return "Connecting…";
    case NM_DEVICE_STATE_NEED_AUTH: return "Waiting for password";
    case NM_DEVICE_STATE_IP_CONFIG:
    case NM_DEVICE_STATE_IP_CHECK: return "Getting an address…";
    case NM_DEVICE_STATE_SECONDARIES: return "Connecting…";
    case NM_DEVICE_STATE_ACTIVATED: return "Connected";
    case NM_DEVICE_STATE_DEACTIVATING: return "Disconnecting…";
    case NM_DEVICE_STATE_FAILED: return "Failed";
    default: return "Unknown";
  }
}

void fill_ip(NMIPConfig *cfg, std::vector<std::string> &out, std::string &gw, std::vector<std::string> *dns) {
  if (!cfg) return;
  GPtrArray *addrs = nm_ip_config_get_addresses(cfg);
  for (guint i = 0; addrs && i < addrs->len; i++) {
    NMIPAddress *a = (NMIPAddress *)g_ptr_array_index(addrs, i);
    out.push_back(str(nm_ip_address_get_address(a)) + "/" + std::to_string(nm_ip_address_get_prefix(a)));
  }
  gw = str(nm_ip_config_get_gateway(cfg));
  if (dns) {
    const char *const *ns = nm_ip_config_get_nameservers(cfg);
    for (int i = 0; ns && ns[i]; i++) dns->push_back(ns[i]);
  }
}

void ap_security(NMAccessPoint *ap, AccessPoint &out) {
  NM80211ApFlags flags = nm_access_point_get_flags(ap);
  NM80211ApSecurityFlags wpa = nm_access_point_get_wpa_flags(ap), rsn = nm_access_point_get_rsn_flags(ap);
  NM80211ApSecurityFlags all = (NM80211ApSecurityFlags)(wpa | rsn);
  out.enterprise = all & NM_802_11_AP_SEC_KEY_MGMT_802_1X;
  out.sae = all & NM_802_11_AP_SEC_KEY_MGMT_SAE;
  bool psk = all & NM_802_11_AP_SEC_KEY_MGMT_PSK;
  out.secure = out.enterprise || out.sae || psk || (flags & NM_802_11_AP_FLAGS_PRIVACY);
  if (out.enterprise) out.security = "802.1X";
  else if (out.sae && psk) out.security = "WPA2/3";
  else if (out.sae) out.security = "WPA3";
  else if (psk) out.security = rsn ? "WPA2" : "WPA";
  else if (flags & NM_802_11_AP_FLAGS_PRIVACY) out.security = "WEP";
}

void watch(gpointer obj);

State snapshot() {
  State s;
  if (!g_client || !nm_client_get_nm_running(g_client)) return s;
  s.running = true;
  s.networking = nm_client_networking_get_enabled(g_client);
  s.wireless = nm_client_wireless_get_enabled(g_client);
  s.wireless_hw = nm_client_wireless_hardware_get_enabled(g_client);
  switch (nm_client_get_connectivity(g_client)) {
    case NM_CONNECTIVITY_FULL: s.connectivity = "full"; break;
    case NM_CONNECTIVITY_LIMITED: s.connectivity = "limited"; break;
    case NM_CONNECTIVITY_PORTAL: s.connectivity = "portal"; break;
    case NM_CONNECTIVITY_NONE: s.connectivity = "none"; break;
    default: s.connectivity = "unknown";
  }
  NMActiveConnection *primary = nm_client_get_primary_connection(g_client);
  if (primary) {
    s.primary_id = str(nm_active_connection_get_id(primary));
    s.primary_type = str(nm_active_connection_get_connection_type(primary));
  }
  std::set<std::string> active_uuids;
  const GPtrArray *acs = nm_client_get_active_connections(g_client);
  for (guint i = 0; acs && i < acs->len; i++)
    active_uuids.insert(str(nm_active_connection_get_uuid((NMActiveConnection *)g_ptr_array_index(acs, i))));

  const GPtrArray *devs = nm_client_get_devices(g_client);
  for (guint i = 0; devs && i < devs->len; i++) {
    NMDevice *d = (NMDevice *)g_ptr_array_index(devs, i);
    watch(d);
    Device dv;
    dv.path = str(nm_object_get_path(NM_OBJECT(d)));
    dv.iface = str(nm_device_get_iface(d));
    dv.type = device_type(d);
    dv.type_description = str(nm_device_get_type_description(d));
    dv.driver = str(nm_device_get_driver(d));
    dv.hwaddr = str(nm_device_get_hw_address(d));
    NMDeviceState st = nm_device_get_state(d);
    dv.state_code = st;
    dv.state = state_text(st);
    dv.managed = nm_device_get_managed(d);
    dv.available = st > NM_DEVICE_STATE_UNAVAILABLE;
    dv.connected = st == NM_DEVICE_STATE_ACTIVATED;
    dv.connecting = st > NM_DEVICE_STATE_DISCONNECTED && st < NM_DEVICE_STATE_ACTIVATED;
    if (NMActiveConnection *ac = nm_device_get_active_connection(d)) {
      dv.active_id = str(nm_active_connection_get_id(ac));
      dv.active_uuid = str(nm_active_connection_get_uuid(ac));
    }
    fill_ip(nm_device_get_ip4_config(d), dv.ip4, dv.gateway4, &dv.dns);
    std::string unused;
    fill_ip(nm_device_get_ip6_config(d), dv.ip6, dv.gateway6, nullptr);
    if (NM_IS_DEVICE_ETHERNET(d)) dv.speed = nm_device_ethernet_get_speed(NM_DEVICE_ETHERNET(d));
    if (NM_IS_DEVICE_WIFI(d)) {
      NMDeviceWifi *w = NM_DEVICE_WIFI(d);
      dv.speed = nm_device_wifi_get_bitrate(w) / 1000;
      NMAccessPoint *active = nm_device_wifi_get_active_access_point(w);
      std::map<std::string, AccessPoint> by_ssid;
      const GPtrArray *aps = nm_device_wifi_get_access_points(w);
      for (guint k = 0; aps && k < aps->len; k++) {
        NMAccessPoint *ap = (NMAccessPoint *)g_ptr_array_index(aps, k);
        watch(ap);
        AccessPoint a;
        a.ssid = ssid_of(nm_access_point_get_ssid(ap));
        if (a.ssid.empty()) continue; // hidden networks
        a.bssid = str(nm_access_point_get_bssid(ap));
        a.path = str(nm_object_get_path(NM_OBJECT(ap)));
        a.strength = nm_access_point_get_strength(ap);
        a.frequency = nm_access_point_get_frequency(ap);
        a.active = ap == active;
        ap_security(ap, a);
        auto it = by_ssid.find(a.ssid);
        if (it == by_ssid.end() || a.active || (!it->second.active && a.strength > it->second.strength))
          by_ssid[a.ssid] = a;
        if (a.active && primary && nm_active_connection_get_devices(primary) &&
            g_ptr_array_find((GPtrArray *)nm_active_connection_get_devices(primary), d, nullptr))
          s.primary_strength = a.strength;
      }
      for (auto &kv : by_ssid) dv.aps.push_back(kv.second);
      std::sort(dv.aps.begin(), dv.aps.end(), [](const AccessPoint &a, const AccessPoint &b) {
        if (a.active != b.active) return a.active;
        return a.strength > b.strength;
      });
    }
    s.devices.push_back(dv);
  }
  const GPtrArray *conns = nm_client_get_connections(g_client);
  for (guint i = 0; conns && i < conns->len; i++) {
    NMRemoteConnection *rc = (NMRemoteConnection *)g_ptr_array_index(conns, i);
    NMConnection *c = NM_CONNECTION(rc);
    Connection cn;
    cn.path = str(nm_connection_get_path(c));
    cn.id = str(nm_connection_get_id(c));
    cn.uuid = str(nm_connection_get_uuid(c));
    cn.type = str(nm_connection_get_connection_type(c));
    cn.iface = str(nm_connection_get_interface_name(c));
    if (NMSettingConnection *sc = nm_connection_get_setting_connection(c)) {
      cn.autoconnect = nm_setting_connection_get_autoconnect(sc);
      cn.timestamp = (long long)nm_setting_connection_get_timestamp(sc);
    }
    if (NMSettingWireless *sw = nm_connection_get_setting_wireless(c)) cn.ssid = ssid_of(nm_setting_wireless_get_ssid(sw));
    cn.active = active_uuids.count(cn.uuid) > 0;
    if (cn.type == "loopback") continue;
    s.connections.push_back(cn);
  }
  std::sort(s.connections.begin(), s.connections.end(), [](const Connection &a, const Connection &b) {
    if (a.active != b.active) return a.active;
    return strcasecmp(a.id.c_str(), b.id.c_str()) < 0;
  });
  return s;
}

gboolean do_refresh(gpointer) {
  g_refresh_source = 0;
  State s = snapshot();
  fld::ui_call([s] {
    Client::get().state_ = s;
    if (Client::get().on_changed) Client::get().on_changed();
  });
  return G_SOURCE_REMOVE;
}

void schedule_refresh() {
  if (!g_refresh_source) g_refresh_source = fld::glib_timeout(120, do_refresh, nullptr);
}

void on_any(GObject *, GParamSpec *, gpointer) { schedule_refresh(); }
void on_any_obj(GObject *, gpointer, gpointer) { schedule_refresh(); }
void on_state_changed(NMDevice *, guint, guint, guint, gpointer) { schedule_refresh(); }

void forget(gpointer, GObject *obj) { g_watched.erase(obj); }

void watch(gpointer obj) {
  if (g_watched.count(obj)) return;
  g_watched.insert(obj);
  g_object_weak_ref(G_OBJECT(obj), forget, nullptr);
  g_signal_connect(obj, "notify", G_CALLBACK(on_any), nullptr);
  if (NM_IS_DEVICE(obj)) g_signal_connect(obj, "state-changed", G_CALLBACK(on_state_changed), nullptr);
  if (NM_IS_DEVICE_WIFI(obj)) {
    g_signal_connect(obj, "access-point-added", G_CALLBACK(on_any_obj), nullptr);
    g_signal_connect(obj, "access-point-removed", G_CALLBACK(on_any_obj), nullptr);
  }
}

NMRemoteConnection *find_conn(const std::string &uuid) {
  return g_client ? nm_client_get_connection_by_uuid(g_client, uuid.c_str()) : nullptr;
}

NMDevice *find_device(const std::string &path) {
  if (!g_client || path.empty()) return nullptr;
  return (NMDevice *)nm_client_get_device_by_path(g_client, path.c_str());
}

// async completion: GError -> Done in the UI thread
struct Op {
  Done done;
};

void report(Op *op, GError *err) {
  std::string e = err ? err->message : "";
  if (err) g_error_free(err);
  Done d = op->done;
  delete op;
  schedule_refresh();
  if (d) fld::ui_call([d, e] { d(e); });
}

std::string nm_error_text(GError *e) { return e ? e->message : ""; }

} // namespace

// ---------------------------------------------------------------------------
// public API (called in the FLTK thread)

Client &Client::get() {
  static Client c;
  return c;
}

void Client::start() {
  fld::glib_start();
  fld::glib_call([] {
    if (g_client) return;
    GError *err = nullptr;
    g_client = nm_client_new(nullptr, &err);
    if (!g_client) {
      fprintf(stderr, "fld-nm: %s\n", nm_error_text(err).c_str());
      if (err) g_error_free(err);
      schedule_refresh();
      return;
    }
    g_signal_connect(g_client, "notify", G_CALLBACK(on_any), nullptr);
    for (const char *sig : {"device-added", "device-removed", "connection-added", "connection-removed",
                            "active-connection-added", "active-connection-removed"})
      g_signal_connect(g_client, sig, G_CALLBACK(on_any_obj), nullptr);
    schedule_refresh();
  });
}

void Client::set_networking(bool on, Done done) {
  fld::glib_call([on, done] {
    if (!g_client) return;
    Op *op = new Op{done};
    nm_client_dbus_call(g_client, NM_DBUS_PATH, NM_DBUS_INTERFACE, "Enable", g_variant_new("(b)", on), nullptr, -1,
                        nullptr,
                        [](GObject *src, GAsyncResult *res, gpointer data) {
                          GError *e = nullptr;
                          GVariant *r = nm_client_dbus_call_finish(NM_CLIENT(src), res, &e);
                          if (r) g_variant_unref(r);
                          report((Op *)data, e);
                        },
                        op);
  });
}

void Client::set_wireless(bool on, Done done) {
  fld::glib_call([on, done] {
    if (!g_client) return;
    Op *op = new Op{done};
    nm_client_dbus_set_property(g_client, NM_DBUS_PATH, NM_DBUS_INTERFACE, "WirelessEnabled",
                                g_variant_new_boolean(on), -1, nullptr,
                                [](GObject *src, GAsyncResult *res, gpointer data) {
                                  GError *e = nullptr;
                                  nm_client_dbus_set_property_finish(NM_CLIENT(src), res, &e);
                                  report((Op *)data, e);
                                },
                                op);
  });
}

void Client::scan(const std::string &device_path) {
  fld::glib_call([device_path] {
    NMDevice *d = find_device(device_path);
    if (d && NM_IS_DEVICE_WIFI(d))
      nm_device_wifi_request_scan_async(NM_DEVICE_WIFI(d), nullptr,
                                        [](GObject *src, GAsyncResult *res, gpointer) {
                                          GError *e = nullptr;
                                          nm_device_wifi_request_scan_finish(NM_DEVICE_WIFI(src), res, &e);
                                          if (e) g_error_free(e);
                                        },
                                        nullptr);
  });
}

static void activate_cb(GObject *src, GAsyncResult *res, gpointer data) {
  GError *e = nullptr;
  NMActiveConnection *ac = nm_client_activate_connection_finish(NM_CLIENT(src), res, &e);
  if (ac) g_object_unref(ac);
  report((Op *)data, e);
}

void Client::activate(const std::string &uuid, const std::string &device_path, Done done) {
  fld::glib_call([uuid, device_path, done] {
    NMRemoteConnection *c = find_conn(uuid);
    if (!c) { report(new Op{done}, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "connection not found")); return; }
    nm_client_activate_connection_async(g_client, NM_CONNECTION(c), find_device(device_path), nullptr, nullptr,
                                        activate_cb, new Op{done});
  });
}

void Client::deactivate(const std::string &uuid, Done done) {
  fld::glib_call([uuid, done] {
    if (!g_client) return;
    const GPtrArray *acs = nm_client_get_active_connections(g_client);
    for (guint i = 0; acs && i < acs->len; i++) {
      NMActiveConnection *ac = (NMActiveConnection *)g_ptr_array_index(acs, i);
      if (uuid != str(nm_active_connection_get_uuid(ac))) continue;
      nm_client_deactivate_connection_async(g_client, ac, nullptr,
                                            [](GObject *src, GAsyncResult *res, gpointer data) {
                                              GError *e = nullptr;
                                              nm_client_deactivate_connection_finish(NM_CLIENT(src), res, &e);
                                              report((Op *)data, e);
                                            },
                                            new Op{done});
      return;
    }
    report(new Op{done}, nullptr);
  });
}

void Client::disconnect_device(const std::string &device_path, Done done) {
  fld::glib_call([device_path, done] {
    NMDevice *d = find_device(device_path);
    if (!d) { report(new Op{done}, nullptr); return; }
    nm_device_disconnect_async(d, nullptr,
                               [](GObject *src, GAsyncResult *res, gpointer data) {
                                 GError *e = nullptr;
                                 nm_device_disconnect_finish(NM_DEVICE(src), res, &e);
                                 report((Op *)data, e);
                               },
                               new Op{done});
  });
}

void Client::connect_wifi(const std::string &device_path, const std::string &ssid, const std::string &password,
                          bool hidden, Done done) {
  fld::glib_call([device_path, ssid, password, hidden, done] {
    if (!g_client) return;
    NMDevice *dev = find_device(device_path);
    // a saved connection for this SSID: just activate it (with a new password if given)
    const GPtrArray *conns = nm_client_get_connections(g_client);
    for (guint i = 0; conns && i < conns->len && password.empty(); i++) {
      NMConnection *c = NM_CONNECTION(g_ptr_array_index(conns, i));
      NMSettingWireless *sw = nm_connection_get_setting_wireless(c);
      if (sw && ssid_of(nm_setting_wireless_get_ssid(sw)) == ssid) {
        nm_client_activate_connection_async(g_client, c, dev, nullptr, nullptr, activate_cb, new Op{done});
        return;
      }
    }
    // find the access point (for its security type)
    NMAccessPoint *ap = nullptr;
    if (dev && NM_IS_DEVICE_WIFI(dev)) {
      const GPtrArray *aps = nm_device_wifi_get_access_points(NM_DEVICE_WIFI(dev));
      for (guint i = 0; aps && i < aps->len; i++) {
        NMAccessPoint *a = (NMAccessPoint *)g_ptr_array_index(aps, i);
        if (ssid_of(nm_access_point_get_ssid(a)) == ssid &&
            (!ap || nm_access_point_get_strength(a) > nm_access_point_get_strength(ap)))
          ap = a;
      }
    }
    NMConnection *partial = nm_simple_connection_new();
    NMSettingWireless *sw = (NMSettingWireless *)nm_setting_wireless_new();
    GBytes *ssid_bytes = g_bytes_new(ssid.data(), ssid.size());
    g_object_set(sw, NM_SETTING_WIRELESS_SSID, ssid_bytes, nullptr);
    if (hidden) g_object_set(sw, NM_SETTING_WIRELESS_HIDDEN, TRUE, nullptr);
    g_bytes_unref(ssid_bytes);
    nm_connection_add_setting(partial, NM_SETTING(sw));
    if (!password.empty()) {
      AccessPoint info;
      if (ap) ap_security(ap, info);
      NMSettingWirelessSecurity *sec = (NMSettingWirelessSecurity *)nm_setting_wireless_security_new();
      g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, info.sae && info.security == "WPA3" ? "sae" : "wpa-psk",
                   NM_SETTING_WIRELESS_SECURITY_PSK, password.c_str(), nullptr);
      nm_connection_add_setting(partial, NM_SETTING(sec));
    }
    if (hidden || !ap) {
      NMSettingConnection *sc = (NMSettingConnection *)nm_setting_connection_new();
      g_object_set(sc, NM_SETTING_CONNECTION_ID, ssid.c_str(), NM_SETTING_CONNECTION_TYPE,
                   NM_SETTING_WIRELESS_SETTING_NAME, nullptr);
      nm_connection_add_setting(partial, NM_SETTING(sc));
    }
    nm_client_add_and_activate_connection_async(
        g_client, partial, dev, ap ? nm_object_get_path(NM_OBJECT(ap)) : nullptr, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
          GError *e = nullptr;
          NMActiveConnection *ac = nm_client_add_and_activate_connection_finish(NM_CLIENT(src), res, &e);
          if (ac) g_object_unref(ac);
          report((Op *)data, e);
        },
        new Op{done});
    g_object_unref(partial);
  });
}

void Client::delete_connection(const std::string &uuid, Done done) {
  fld::glib_call([uuid, done] {
    NMRemoteConnection *c = find_conn(uuid);
    if (!c) { report(new Op{done}, nullptr); return; }
    nm_remote_connection_delete_async(c, nullptr,
                                      [](GObject *src, GAsyncResult *res, gpointer data) {
                                        GError *e = nullptr;
                                        nm_remote_connection_delete_finish(NM_REMOTE_CONNECTION(src), res, &e);
                                        report((Op *)data, e);
                                      },
                                      new Op{done});
  });
}

// ---- settings ----------------------------------------------------------------

namespace {

std::string ip_addresses(NMSettingIPConfig *ip) {
  std::string out;
  for (guint i = 0; i < nm_setting_ip_config_get_num_addresses(ip); i++) {
    NMIPAddress *a = nm_setting_ip_config_get_address(ip, i);
    if (!out.empty()) out += "\n";
    out += str(nm_ip_address_get_address(a)) + "/" + std::to_string(nm_ip_address_get_prefix(a));
  }
  return out;
}

std::string ip_dns(NMSettingIPConfig *ip) {
  std::string out;
  for (guint i = 0; i < nm_setting_ip_config_get_num_dns(ip); i++) {
    if (!out.empty()) out += " ";
    out += str(nm_setting_ip_config_get_dns(ip, i));
  }
  return out;
}

Settings read_settings(NMConnection *c) {
  Settings s;
  s.uuid = str(nm_connection_get_uuid(c));
  s.id = str(nm_connection_get_id(c));
  s.type = str(nm_connection_get_connection_type(c));
  s.iface = str(nm_connection_get_interface_name(c));
  if (NMSettingConnection *sc = nm_connection_get_setting_connection(c)) {
    s.autoconnect = nm_setting_connection_get_autoconnect(sc);
    s.all_users = nm_setting_connection_get_num_permissions(sc) == 0;
  }
  if (NMSettingWireless *sw = nm_connection_get_setting_wireless(c)) {
    s.ssid = ssid_of(nm_setting_wireless_get_ssid(sw));
    s.hidden = nm_setting_wireless_get_hidden(sw);
  }
  if (NMSettingWirelessSecurity *sec = nm_connection_get_setting_wireless_security(c)) {
    std::string km = str(nm_setting_wireless_security_get_key_mgmt(sec));
    s.security = km == "sae" ? "sae" : km == "wpa-psk" ? "wpa-psk" : km.empty() || km == "none" ? "none" : km;
  }
  if (NMSetting *ws = nm_connection_get_setting(c, NM_TYPE_SETTING_WIREGUARD)) {
    NMSettingWireGuard *wg = NM_SETTING_WIREGUARD(ws);
    s.wg_private_key = str(nm_setting_wireguard_get_private_key(wg));
    s.wg_has_private_key = !s.wg_private_key.empty() ||
                           nm_setting_wireguard_get_private_key_flags(wg) != NM_SETTING_SECRET_FLAG_NOT_REQUIRED;
    s.wg_listen_port = nm_setting_wireguard_get_listen_port(wg);
    s.wg_mtu = (int)nm_setting_wireguard_get_mtu(wg);
    for (guint i = 0; i < nm_setting_wireguard_get_peers_len(wg); i++) {
      NMWireGuardPeer *p = nm_setting_wireguard_get_peer(wg, i);
      WgPeer peer;
      peer.public_key = str(nm_wireguard_peer_get_public_key(p));
      peer.preshared_key = str(nm_wireguard_peer_get_preshared_key(p));
      peer.endpoint = str(nm_wireguard_peer_get_endpoint(p));
      peer.keepalive = nm_wireguard_peer_get_persistent_keepalive(p);
      for (guint k = 0; k < nm_wireguard_peer_get_allowed_ips_len(p); k++)
        peer.allowed_ips += (k ? "," : "") + str(nm_wireguard_peer_get_allowed_ip(p, k, nullptr));
      s.wg_peers.push_back(peer);
    }
  }
  if (NMSettingVpn *vpn = nm_connection_get_setting_vpn(c)) {
    s.vpn_service = str(nm_setting_vpn_get_service_type(vpn));
    s.vpn_user = str(nm_setting_vpn_get_user_name(vpn));
    guint n = 0;
    const char **keys = nm_setting_vpn_get_data_keys(vpn, &n);
    for (guint i = 0; keys && i < n; i++) s.vpn_data.push_back({keys[i], str(nm_setting_vpn_get_data_item(vpn, keys[i]))});
    g_free(keys);
    s.vpn_password = str(nm_setting_vpn_get_secret(vpn, "password"));
  }
  if (NMSettingIPConfig *ip4 = nm_connection_get_setting_ip4_config(c)) {
    s.ip4_method = str(nm_setting_ip_config_get_method(ip4));
    s.ip4_addresses = ip_addresses(ip4);
    s.ip4_gateway = str(nm_setting_ip_config_get_gateway(ip4));
    s.ip4_dns = ip_dns(ip4);
    s.ip4_ignore_auto_dns = nm_setting_ip_config_get_ignore_auto_dns(ip4);
  }
  if (NMSettingIPConfig *ip6 = nm_connection_get_setting_ip6_config(c)) {
    s.ip6_method = str(nm_setting_ip_config_get_method(ip6));
    s.ip6_addresses = ip_addresses(ip6);
    s.ip6_gateway = str(nm_setting_ip_config_get_gateway(ip6));
    s.ip6_dns = ip_dns(ip6);
    s.ip6_ignore_auto_dns = nm_setting_ip_config_get_ignore_auto_dns(ip6);
  }
  return s;
}

// writes the editable values into a connection; returns an error text or ""
std::string apply_ip(NMConnection *c, bool v6, const std::string &method, const std::string &addresses,
                     const std::string &gateway, const std::string &dns, bool ignore_auto_dns) {
  NMSettingIPConfig *ip = v6 ? nm_connection_get_setting_ip6_config(c) : nm_connection_get_setting_ip4_config(c);
  if (!ip) {
    ip = v6 ? (NMSettingIPConfig *)nm_setting_ip6_config_new() : (NMSettingIPConfig *)nm_setting_ip4_config_new();
    nm_connection_add_setting(c, NM_SETTING(ip));
  }
  nm_setting_ip_config_clear_addresses(ip);
  nm_setting_ip_config_clear_dns(ip);
  g_object_set(ip, NM_SETTING_IP_CONFIG_METHOD, method.c_str(), NM_SETTING_IP_CONFIG_GATEWAY,
               gateway.empty() || method != "manual" ? nullptr : gateway.c_str(), NM_SETTING_IP_CONFIG_IGNORE_AUTO_DNS,
               ignore_auto_dns, nullptr);
  if (method == "manual") {
    for (auto &a : split_list(addresses)) {
      std::string addr;
      unsigned prefix;
      if (!parse_address(a, addr, prefix, v6)) return "Invalid " + std::string(v6 ? "IPv6" : "IPv4") + " address: " + a;
      GError *e = nullptr;
      NMIPAddress *ia = nm_ip_address_new(v6 ? AF_INET6 : AF_INET, addr.c_str(), prefix, &e);
      if (!ia) {
        std::string m = e ? e->message : "invalid address";
        if (e) g_error_free(e);
        return m;
      }
      nm_setting_ip_config_add_address(ip, ia);
      nm_ip_address_unref(ia);
    }
    if (nm_setting_ip_config_get_num_addresses(ip) == 0)
      return std::string("Manual ") + (v6 ? "IPv6" : "IPv4") + " configuration needs an address";
    if (!gateway.empty()) {
      std::string g;
      unsigned p;
      if (!parse_address(gateway, g, p, v6)) return "Invalid gateway: " + gateway;
    }
  }
  for (auto &d : split_list(dns)) {
    std::string a;
    unsigned p;
    if (!parse_address(d, a, p, v6)) return "Invalid DNS server: " + d;
    nm_setting_ip_config_add_dns(ip, d.c_str());
  }
  return "";
}

std::string apply_settings(NMConnection *c, const Settings &s) {
  NMSettingConnection *sc = nm_connection_get_setting_connection(c);
  if (!sc) {
    sc = (NMSettingConnection *)nm_setting_connection_new();
    nm_connection_add_setting(c, NM_SETTING(sc));
  }
  if (s.id.empty()) return "The connection needs a name";
  g_object_set(sc, NM_SETTING_CONNECTION_ID, s.id.c_str(), NM_SETTING_CONNECTION_AUTOCONNECT, s.autoconnect,
               NM_SETTING_CONNECTION_INTERFACE_NAME, s.iface.empty() ? nullptr : s.iface.c_str(), nullptr);
  if (!nm_setting_connection_get_uuid(sc)) {
    char *u = nm_utils_uuid_generate();
    g_object_set(sc, NM_SETTING_CONNECTION_UUID, u, nullptr);
    g_free(u);
  }
  if (!nm_setting_connection_get_connection_type(sc))
    g_object_set(sc, NM_SETTING_CONNECTION_TYPE, s.type.c_str(), nullptr);
  // all users = no permissions; otherwise only the current user
  g_object_set(sc, NM_SETTING_CONNECTION_PERMISSIONS, nullptr, nullptr);
  if (!s.all_users) {
    const char *user = g_get_user_name();
    nm_setting_connection_add_permission(sc, "user", user, nullptr);
  }
  if (s.type == NM_SETTING_WIRELESS_SETTING_NAME) {
    NMSettingWireless *sw = nm_connection_get_setting_wireless(c);
    if (!sw) {
      sw = (NMSettingWireless *)nm_setting_wireless_new();
      nm_connection_add_setting(c, NM_SETTING(sw));
    }
    if (s.ssid.empty()) return "The Wi-Fi connection needs an SSID";
    GBytes *b = g_bytes_new(s.ssid.data(), s.ssid.size());
    g_object_set(sw, NM_SETTING_WIRELESS_SSID, b, NM_SETTING_WIRELESS_HIDDEN, s.hidden, nullptr);
    g_bytes_unref(b);
    if (s.security == "none") {
      nm_connection_remove_setting(c, NM_TYPE_SETTING_WIRELESS_SECURITY);
    } else if (s.security == "wpa-psk" || s.security == "sae") {
      NMSettingWirelessSecurity *sec = nm_connection_get_setting_wireless_security(c);
      if (!sec) {
        sec = (NMSettingWirelessSecurity *)nm_setting_wireless_security_new();
        nm_connection_add_setting(c, NM_SETTING(sec));
      }
      g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_KEY_MGMT, s.security.c_str(), nullptr);
      if (!s.psk.empty()) {
        if (s.psk.size() < 8) return "Wi-Fi passwords have at least 8 characters";
        g_object_set(sec, NM_SETTING_WIRELESS_SECURITY_PSK, s.psk.c_str(), nullptr);
      }
    }
  } else if (s.type == NM_SETTING_WIRED_SETTING_NAME && !nm_connection_get_setting_wired(c)) {
    nm_connection_add_setting(c, nm_setting_wired_new());
  } else if (s.type == NM_SETTING_WIREGUARD_SETTING_NAME) {
    NMSetting *ws = nm_connection_get_setting(c, NM_TYPE_SETTING_WIREGUARD);
    if (!ws) {
      ws = nm_setting_wireguard_new();
      nm_connection_add_setting(c, ws);
    }
    NMSettingWireGuard *wg = NM_SETTING_WIREGUARD(ws);
    if (s.iface.empty()) return "A WireGuard connection needs an interface name (Device), e.g. wg0";
    if (!s.wg_private_key.empty()) {
      if (!valid_wg_key(s.wg_private_key)) return "The private key is not a WireGuard key";
      g_object_set(wg, NM_SETTING_WIREGUARD_PRIVATE_KEY, s.wg_private_key.c_str(),
                   NM_SETTING_WIREGUARD_PRIVATE_KEY_FLAGS, NM_SETTING_SECRET_FLAG_NONE, nullptr);
    } else if (!s.wg_has_private_key) {
      return "The WireGuard connection needs a private key";
    }
    g_object_set(wg, NM_SETTING_WIREGUARD_LISTEN_PORT, (guint)std::max(0, s.wg_listen_port), NM_SETTING_WIREGUARD_MTU,
                 (guint)std::max(0, s.wg_mtu), nullptr);
    nm_setting_wireguard_clear_peers(wg);
    for (auto &p : s.wg_peers) {
      if (!valid_wg_key(p.public_key)) return "A peer has no valid public key";
      NMWireGuardPeer *peer = nm_wireguard_peer_new();
      nm_wireguard_peer_set_public_key(peer, p.public_key.c_str(), TRUE);
      if (!p.endpoint.empty()) nm_wireguard_peer_set_endpoint(peer, p.endpoint.c_str(), TRUE);
      if (!p.preshared_key.empty()) {
        if (!valid_wg_key(p.preshared_key)) {
          nm_wireguard_peer_unref(peer);
          return "A peer's pre-shared key is not valid";
        }
        nm_wireguard_peer_set_preshared_key(peer, p.preshared_key.c_str(), TRUE);
        nm_wireguard_peer_set_preshared_key_flags(peer, NM_SETTING_SECRET_FLAG_NONE);
      }
      nm_wireguard_peer_set_persistent_keepalive(peer, (guint16)std::max(0, std::min(65535, p.keepalive)));
      for (auto &a : split_list(p.allowed_ips)) nm_wireguard_peer_append_allowed_ip(peer, a.c_str(), TRUE);
      GError *pe = nullptr;
      if (!nm_wireguard_peer_is_valid(peer, TRUE, TRUE, &pe)) {
        std::string m = std::string("Peer ") + p.public_key.substr(0, 8) + "…: " + (pe ? pe->message : "invalid");
        if (pe) g_error_free(pe);
        nm_wireguard_peer_unref(peer);
        return m;
      }
      nm_setting_wireguard_append_peer(wg, peer);
      nm_wireguard_peer_unref(peer);
    }
  } else if (s.type == NM_SETTING_VPN_SETTING_NAME) {
    NMSettingVpn *vpn = nm_connection_get_setting_vpn(c);
    if (!vpn) {
      vpn = (NMSettingVpn *)nm_setting_vpn_new();
      nm_connection_add_setting(c, NM_SETTING(vpn));
    }
    if (s.vpn_service.empty()) return "The VPN connection has no VPN type";
    g_object_set(vpn, NM_SETTING_VPN_SERVICE_TYPE, s.vpn_service.c_str(), NM_SETTING_VPN_USER_NAME,
                 s.vpn_user.empty() ? nullptr : s.vpn_user.c_str(), nullptr);
    guint n = 0;
    const char **keys = nm_setting_vpn_get_data_keys(vpn, &n);
    std::vector<std::string> old;
    for (guint i = 0; keys && i < n; i++) old.push_back(keys[i]);
    g_free(keys);
    for (auto &k : old) nm_setting_vpn_remove_data_item(vpn, k.c_str());
    for (auto &kv : s.vpn_data) nm_setting_vpn_add_data_item(vpn, kv.first.c_str(), kv.second.c_str());
    if (!s.vpn_password.empty()) {
      // stored by NetworkManager for everyone who may use the connection
      nm_setting_vpn_add_secret(vpn, "password", s.vpn_password.c_str());
      nm_setting_vpn_add_data_item(vpn, "password-flags", "0");
    }
  }
  std::string e = apply_ip(c, false, s.ip4_method, s.ip4_addresses, s.ip4_gateway, s.ip4_dns, s.ip4_ignore_auto_dns);
  if (!e.empty()) return e;
  return apply_ip(c, true, s.ip6_method, s.ip6_addresses, s.ip6_gateway, s.ip6_dns, s.ip6_ignore_auto_dns);
}

} // namespace

void Client::get_settings(const std::string &uuid, std::function<void(const Settings &, const std::string &)> cb) {
  fld::glib_call([uuid, cb] {
    NMRemoteConnection *c = find_conn(uuid);
    if (!c) {
      fld::ui_call([cb] { cb(Settings(), "connection not found"); });
      return;
    }
    // WireGuard keys and VPN passwords are secrets: fetch them so the editor
    // shows (and saves) them
    const char *secret_setting = nullptr;
    if (nm_connection_get_setting(NM_CONNECTION(c), NM_TYPE_SETTING_WIREGUARD)) secret_setting = NM_SETTING_WIREGUARD_SETTING_NAME;
    else if (nm_connection_get_setting_vpn(NM_CONNECTION(c))) secret_setting = NM_SETTING_VPN_SETTING_NAME;
    if (!secret_setting) {
      Settings s = read_settings(NM_CONNECTION(c));
      fld::ui_call([cb, s] { cb(s, ""); });
      return;
    }
    struct Req {
      std::function<void(const Settings &, const std::string &)> cb;
      std::string setting;
    };
    nm_remote_connection_get_secrets_async(
        c, secret_setting, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
          Req *r = (Req *)data;
          NMConnection *conn = NM_CONNECTION(src);
          GError *e = nullptr;
          GVariant *secrets = nm_remote_connection_get_secrets_finish(NM_REMOTE_CONNECTION(src), res, &e);
          Settings s;
          if (secrets) {
            NMConnection *copy = nm_simple_connection_new_clone(conn);
            nm_connection_update_secrets(copy, r->setting.c_str(), secrets, nullptr);
            s = read_settings(copy);
            g_object_unref(copy);
            g_variant_unref(secrets);
          } else {
            // not allowed to read them: they stay as they are when saving
            s = read_settings(conn);
          }
          g_clear_error(&e);
          auto cb = r->cb;
          delete r;
          fld::ui_call([cb, s] { cb(s, ""); });
        },
        new Req{cb, secret_setting});
  });
}

std::vector<VpnPlugin> vpn_plugins() {
  std::vector<VpnPlugin> v;
  GSList *list = nm_vpn_plugin_info_list_load();
  for (GSList *l = list; l; l = l->next) {
    NMVpnPluginInfo *info = NM_VPN_PLUGIN_INFO(l->data);
    VpnPlugin p;
    p.service = str(nm_vpn_plugin_info_get_service(info));
    p.name = str(nm_vpn_plugin_info_get_name(info));
    if (!p.service.empty()) v.push_back(p);
  }
  g_slist_free_full(list, g_object_unref);
  return v;
}

namespace {

// adds a connection (normalized), reports err / uuid in the FLTK thread
void add_new_connection(NMConnection *c, std::function<void(const std::string &, const std::string &)> done) {
  GError *ve = nullptr;
  if (!nm_connection_normalize(c, nullptr, nullptr, &ve)) {
    std::string err = ve ? ve->message : "invalid connection";
    if (ve) g_error_free(ve);
    g_object_unref(c);
    fld::ui_call([done, err] { done(err, ""); });
    return;
  }
  std::string uuid = str(nm_connection_get_uuid(c));
  GVariant *dict = nm_connection_to_dbus(c, NM_CONNECTION_SERIALIZE_ALL);
  g_object_unref(c);
  struct Req {
    std::function<void(const std::string &, const std::string &)> done;
    std::string uuid;
  };
  nm_client_add_connection2(g_client, dict, NM_SETTINGS_ADD_CONNECTION2_FLAG_TO_DISK, nullptr, TRUE, nullptr,
                            [](GObject *src, GAsyncResult *res, gpointer data) {
                              Req *r = (Req *)data;
                              GError *e = nullptr;
                              NMRemoteConnection *rc = nm_client_add_connection2_finish(NM_CLIENT(src), res, nullptr, &e);
                              if (rc) g_object_unref(rc);
                              std::string err = e ? e->message : "";
                              g_clear_error(&e);
                              auto done = r->done;
                              std::string uuid = r->uuid;
                              delete r;
                              fld::ui_call([done, err, uuid] { done(err, err.empty() ? uuid : ""); });
                            },
                            new Req{done, uuid});
}

std::string file_text(const std::string &path) {
  gchar *data = nullptr;
  gsize len = 0;
  std::string s;
  if (g_file_get_contents(path.c_str(), &data, &len, nullptr)) s.assign(data, len);
  g_free(data);
  return s;
}

} // namespace

void Client::import_vpn(const std::string &path,
                        std::function<void(const std::string &err, const std::string &uuid)> done) {
  fld::glib_call([path, done] {
    if (!g_client) {
      fld::ui_call([done] { done("NetworkManager is not running", ""); });
      return;
    }
    std::string text = file_text(path);
    // WireGuard (wg-quick) files
    if (text.find("[Interface]") != std::string::npos) {
      WgConfig wc;
      std::string err;
      if (!parse_wg_quick(text, wc, err)) {
        fld::ui_call([done, err] { done("Not a usable WireGuard file: " + err, ""); });
        return;
      }
      Settings s;
      s.type = NM_SETTING_WIREGUARD_SETTING_NAME;
      s.iface = wg_iface_name(path);
      s.id = s.iface;
      s.wg_private_key = wc.private_key;
      s.wg_listen_port = wc.listen_port;
      s.wg_mtu = wc.mtu;
      s.wg_peers = wc.peers;
      for (auto &a : wc.addresses) {
        std::string &dst = is_ipv6(a) ? s.ip6_addresses : s.ip4_addresses;
        dst += (dst.empty() ? "" : "\n") + a;
      }
      for (auto &d : wc.dns) {
        std::string &dst = is_ipv6(d) ? s.ip6_dns : s.ip4_dns;
        dst += (dst.empty() ? "" : " ") + d;
      }
      s.ip4_method = s.ip4_addresses.empty() ? "disabled" : "manual";
      s.ip6_method = s.ip6_addresses.empty() ? "disabled" : "manual";
      // the VPN's DNS servers take over while it is up
      s.ip4_ignore_auto_dns = !s.ip4_dns.empty();
      s.ip6_ignore_auto_dns = !s.ip6_dns.empty();
      s.autoconnect = false;
      NMConnection *c = nm_simple_connection_new();
      err = apply_settings(c, s);
      if (!err.empty()) {
        g_object_unref(c);
        fld::ui_call([done, err] { done(err, ""); });
        return;
      }
      add_new_connection(c, done);
      return;
    }
    // the installed VPN plugins' importers (.ovpn, .pcf, ...)
    std::string last_err = "No installed VPN plugin can read this file";
    GSList *list = nm_vpn_plugin_info_list_load();
    NMConnection *imported = nullptr;
    for (GSList *l = list; l && !imported; l = l->next) {
      GError *e = nullptr;
      NMVpnEditorPlugin *ed = nm_vpn_plugin_info_load_editor_plugin(NM_VPN_PLUGIN_INFO(l->data), &e);
      if (!ed) {
        g_clear_error(&e);
        continue;
      }
      imported = nm_vpn_editor_plugin_import(ed, path.c_str(), &e);
      if (!imported && e) last_err = e->message;
      g_clear_error(&e);
    }
    g_slist_free_full(list, g_object_unref);
    if (!imported) {
      fld::ui_call([done, last_err] { done(last_err, ""); });
      return;
    }
    NMSettingConnection *sc = nm_connection_get_setting_connection(imported);
    if (sc && !nm_setting_connection_get_id(sc)) {
      gchar *base = g_path_get_basename(path.c_str());
      g_object_set(sc, NM_SETTING_CONNECTION_ID, base, nullptr);
      g_free(base);
    }
    add_new_connection(imported, done);
  });
}

void Client::save_settings(const Settings &s, Done done) {
  fld::glib_call([s, done] {
    if (!g_client) return;
    NMRemoteConnection *remote = s.uuid.empty() ? nullptr : find_conn(s.uuid);
    NMConnection *c = remote ? nm_simple_connection_new_clone(NM_CONNECTION(remote)) : nm_simple_connection_new();
    std::string err = apply_settings(c, s);
    GError *ve = nullptr;
    if (err.empty() && !nm_connection_normalize(c, nullptr, nullptr, &ve)) {
      err = ve ? ve->message : "invalid connection";
      if (ve) g_error_free(ve);
    }
    if (!err.empty()) {
      g_object_unref(c);
      report(new Op{done}, g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, err.c_str()));
      return;
    }
    GVariant *dict = nm_connection_to_dbus(c, NM_CONNECTION_SERIALIZE_ALL);
    if (remote) {
      nm_remote_connection_update2(remote, dict, NM_SETTINGS_UPDATE2_FLAG_TO_DISK, nullptr, nullptr,
                                   [](GObject *src, GAsyncResult *res, gpointer data) {
                                     GError *e = nullptr;
                                     GVariant *r = nm_remote_connection_update2_finish(NM_REMOTE_CONNECTION(src), res, &e);
                                     if (r) g_variant_unref(r);
                                     report((Op *)data, e);
                                   },
                                   new Op{done});
    } else {
      nm_client_add_connection2(g_client, dict, NM_SETTINGS_ADD_CONNECTION2_FLAG_TO_DISK, nullptr, TRUE, nullptr,
                                [](GObject *src, GAsyncResult *res, gpointer data) {
                                  GError *e = nullptr;
                                  NMRemoteConnection *rc = nm_client_add_connection2_finish(NM_CLIENT(src), res, nullptr, &e);
                                  if (rc) g_object_unref(rc);
                                  report((Op *)data, e);
                                },
                                new Op{done});
    }
    g_object_unref(c);
  });
}

bool Client::has_saved_wifi(const std::string &ssid) const {
  for (auto &c : state_.connections)
    if (c.ssid == ssid) return true;
  return false;
}

// ---------------------------------------------------------------------------
// secret agent (Wi-Fi passwords)

namespace {

using AskFn = std::function<void(const std::string &, std::function<void(const std::string &)>)>;
AskFn g_ask;

typedef struct {
  NMSecretAgentOld parent;
} FlSecretAgent;
typedef struct {
  NMSecretAgentOldClass parent_class;
} FlSecretAgentClass;

G_DEFINE_TYPE(FlSecretAgent, fl_secret_agent, NM_TYPE_SECRET_AGENT_OLD)

void agent_get_secrets(NMSecretAgentOld *agent, NMConnection *connection, const char *connection_path,
                       const char *setting_name, const char **hints, NMSecretAgentGetSecretsFlags flags,
                       NMSecretAgentOldGetSecretsFunc callback, gpointer callback_data) {
  bool wifi = setting_name && !strcmp(setting_name, NM_SETTING_WIRELESS_SECURITY_SETTING_NAME);
  NMSettingWirelessSecurity *sec = nm_connection_get_setting_wireless_security(connection);
  std::string km = sec ? str(nm_setting_wireless_security_get_key_mgmt(sec)) : "";
  bool interactive = flags & NM_SECRET_AGENT_GET_SECRETS_FLAG_ALLOW_INTERACTION;
  // VPN plugins: ask for the password (the secret named by the hints)
  if (setting_name && !strcmp(setting_name, NM_SETTING_VPN_SETTING_NAME) && interactive && g_ask) {
    std::string key = "password";
    for (int i = 0; hints && hints[i]; i++)
      if (strncmp(hints[i], "x-", 2)) {
        key = hints[i];
        break;
      }
    std::string name = "VPN " + str(nm_connection_get_id(connection));
    g_object_ref(agent);
    g_object_ref(connection);
    AskFn ask = g_ask;
    fld::ui_call([ask, name, key, agent, connection, callback, callback_data] {
      ask(name, [key, agent, connection, callback, callback_data](const std::string &password) {
        fld::glib_call([key, agent, connection, callback, callback_data, password] {
          if (password.empty()) {
            GError *e = g_error_new_literal(secret_agent_error(), NM_SECRET_AGENT_ERROR_USER_CANCELED, "cancelled");
            callback(agent, connection, nullptr, e, callback_data);
            g_error_free(e);
          } else {
            GVariantBuilder sec_b, inner, outer;
            g_variant_builder_init(&sec_b, G_VARIANT_TYPE("a{ss}"));
            g_variant_builder_add(&sec_b, "{ss}", key.c_str(), password.c_str());
            g_variant_builder_init(&inner, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&inner, "{sv}", NM_SETTING_VPN_SECRETS, g_variant_builder_end(&sec_b));
            g_variant_builder_init(&outer, G_VARIANT_TYPE("a{sa{sv}}"));
            g_variant_builder_add(&outer, "{sa{sv}}", NM_SETTING_VPN_SETTING_NAME, &inner);
            GVariant *secrets = g_variant_ref_sink(g_variant_builder_end(&outer));
            callback(agent, connection, secrets, nullptr, callback_data);
            g_variant_unref(secrets);
          }
          g_object_unref(connection);
          g_object_unref(agent);
        });
      });
    });
    return;
  }
  if (!wifi || !(km == "wpa-psk" || km == "sae") || !interactive || !g_ask) {
    GError *e = g_error_new_literal(secret_agent_error(), NM_SECRET_AGENT_ERROR_NO_SECRETS, "no secrets");
    callback(agent, connection, nullptr, e, callback_data);
    g_error_free(e);
    return;
  }
  NMSettingWireless *sw = nm_connection_get_setting_wireless(connection);
  std::string ssid = sw ? ssid_of(nm_setting_wireless_get_ssid(sw)) : "";
  g_object_ref(agent);
  g_object_ref(connection);
  AskFn ask = g_ask;
  fld::ui_call([ask, ssid, agent, connection, callback, callback_data] {
    ask(ssid, [agent, connection, callback, callback_data](const std::string &password) {
      fld::glib_call([agent, connection, callback, callback_data, password] {
        if (password.empty()) {
          GError *e = g_error_new_literal(secret_agent_error(), NM_SECRET_AGENT_ERROR_USER_CANCELED, "cancelled");
          callback(agent, connection, nullptr, e, callback_data);
          g_error_free(e);
        } else {
          GVariantBuilder inner, outer;
          g_variant_builder_init(&inner, G_VARIANT_TYPE("a{sv}"));
          g_variant_builder_add(&inner, "{sv}", NM_SETTING_WIRELESS_SECURITY_PSK, g_variant_new_string(password.c_str()));
          g_variant_builder_init(&outer, G_VARIANT_TYPE("a{sa{sv}}"));
          g_variant_builder_add(&outer, "{sa{sv}}", NM_SETTING_WIRELESS_SECURITY_SETTING_NAME, &inner);
          GVariant *secrets = g_variant_ref_sink(g_variant_builder_end(&outer));
          callback(agent, connection, secrets, nullptr, callback_data);
          g_variant_unref(secrets);
        }
        g_object_unref(connection);
        g_object_unref(agent);
      });
    });
  });
}

void agent_cancel_get_secrets(NMSecretAgentOld *, const char *, const char *) {}

void agent_save_secrets(NMSecretAgentOld *agent, NMConnection *connection, const char *,
                        NMSecretAgentOldSaveSecretsFunc callback, gpointer data) {
  // secrets are stored system-wide by NetworkManager (not agent-owned)
  callback(agent, connection, nullptr, data);
}

void agent_delete_secrets(NMSecretAgentOld *agent, NMConnection *connection, const char *,
                          NMSecretAgentOldDeleteSecretsFunc callback, gpointer data) {
  callback(agent, connection, nullptr, data);
}

void fl_secret_agent_init(FlSecretAgent *) {}

void fl_secret_agent_class_init(FlSecretAgentClass *klass) {
  NMSecretAgentOldClass *c = NM_SECRET_AGENT_OLD_CLASS(klass);
  c->get_secrets = agent_get_secrets;
  c->cancel_get_secrets = agent_cancel_get_secrets;
  c->save_secrets = agent_save_secrets;
  c->delete_secrets = agent_delete_secrets;
}

} // namespace

void Client::start_secret_agent(AskFn ask) {
  start();
  fld::glib_call([ask] {
    g_ask = ask;
    GError *e = nullptr;
    GObject *agent = (GObject *)g_initable_new(fl_secret_agent_get_type(), nullptr, &e, NM_SECRET_AGENT_OLD_IDENTIFIER,
                                               "org.fldesktop.network-agent", nullptr);
    if (!agent) {
      fprintf(stderr, "fld-nm: secret agent: %s\n", e ? e->message : "?");
      if (e) g_error_free(e);
    }
    // the agent lives as long as the program
  });
}

} // namespace nm
