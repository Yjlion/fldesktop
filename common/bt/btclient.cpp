#include "btclient.h"

#include <fcntl.h>
#include <gio/gio.h>
#include <linux/rfkill.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>

#include "fld/glib_bridge.h"
#include "fld/util.h"

namespace bt {

const Device *State::device(const std::string &path) const {
  for (auto &d : devices)
    if (d.path == path) return &d;
  return nullptr;
}

int State::connected_count() const {
  int n = 0;
  for (auto &d : devices)
    if (d.connected) n++;
  return n;
}

namespace {

// ---- GLib thread state --------------------------------------------------------
GDBusConnection *g_bus = nullptr;
GDBusObjectManager *g_om = nullptr;
bool g_running = false;
guint g_rebuild_src = 0;
int g_discovery_users = 0;
bool g_discovering_by_us = false;
std::map<std::string, bool> g_was_connected; // device path -> connected (for on_connection)

const char *kAgentPath = "/org/fldesktop/bluetooth/agent";

GBusType bus_type() {
  const char *b = getenv("FLD_BLUEZ_BUS");
  return b && !strcmp(b, "session") ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
}

std::string prop_string(GDBusProxy *p, const char *name) {
  GVariant *v = g_dbus_proxy_get_cached_property(p, name);
  std::string s;
  if (v && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)))
    s = g_variant_get_string(v, nullptr);
  if (v) g_variant_unref(v);
  return s;
}

bool prop_bool(GDBusProxy *p, const char *name) {
  GVariant *v = g_dbus_proxy_get_cached_property(p, name);
  bool b = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
  if (v) g_variant_unref(v);
  return b;
}

GDBusProxy *iface(GDBusObject *o, const char *name) {
  return G_DBUS_PROXY(g_dbus_object_get_interface(o, name)); // new ref or null
}

std::string err_from(GError *e) {
  if (!e) return "";
  gchar *remote = g_dbus_error_get_remote_error(e);
  std::string name = remote ? remote : "";
  g_free(remote);
  g_dbus_error_strip_remote_error(e);
  return error_text(name, e->message);
}

void finish(Done done, const std::string &err) {
  if (done) fld::ui_call([done, err] { done(err); });
}

State build_state() {
  State s;
  s.running = g_running && g_om;
  bool soft = false, hard = false;
  rfkill_state("/sys/class/rfkill", soft, hard);
  s.rfkill_blocked = soft || hard;
  s.rfkill_hard = hard;
  if (!g_om) return s;
  GList *objs = g_dbus_object_manager_get_objects(g_om);
  for (GList *l = objs; l; l = l->next) {
    GDBusObject *o = G_DBUS_OBJECT(l->data);
    std::string path = g_dbus_object_get_object_path(o);
    if (GDBusProxy *a = iface(o, "org.bluez.Adapter1")) {
      Adapter ad;
      ad.path = path;
      ad.address = prop_string(a, "Address");
      ad.name = prop_string(a, "Name");
      ad.alias = prop_string(a, "Alias");
      ad.powered = prop_bool(a, "Powered");
      ad.discoverable = prop_bool(a, "Discoverable");
      ad.pairable = prop_bool(a, "Pairable");
      ad.discovering = prop_bool(a, "Discovering");
      s.adapters.push_back(ad);
      g_object_unref(a);
    }
    if (GDBusProxy *dp = iface(o, "org.bluez.Device1")) {
      Device d;
      d.path = path;
      d.adapter = prop_string(dp, "Adapter");
      d.address = prop_string(dp, "Address");
      d.name = prop_string(dp, "Name");
      d.alias = prop_string(dp, "Alias");
      d.icon = prop_string(dp, "Icon");
      d.paired = prop_bool(dp, "Paired");
      d.bonded = prop_bool(dp, "Bonded");
      d.trusted = prop_bool(dp, "Trusted");
      d.connected = prop_bool(dp, "Connected");
      d.blocked = prop_bool(dp, "Blocked");
      if (GVariant *v = g_dbus_proxy_get_cached_property(dp, "RSSI")) {
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_INT16)) {
          d.rssi = g_variant_get_int16(v);
          d.has_rssi = true;
        }
        g_variant_unref(v);
      }
      if (GVariant *v = g_dbus_proxy_get_cached_property(dp, "Class")) {
        if (g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32)) d.device_class = g_variant_get_uint32(v);
        g_variant_unref(v);
      }
      if (GDBusProxy *b = iface(o, "org.bluez.Battery1")) {
        if (GVariant *v = g_dbus_proxy_get_cached_property(b, "Percentage")) {
          if (g_variant_is_of_type(v, G_VARIANT_TYPE_BYTE)) d.battery = g_variant_get_byte(v);
          g_variant_unref(v);
        }
        g_object_unref(b);
      }
      s.devices.push_back(d);
      g_object_unref(dp);
    }
  }
  g_list_free_full(objs, g_object_unref);
  std::sort(s.adapters.begin(), s.adapters.end(), [](const Adapter &a, const Adapter &b) { return a.path < b.path; });
  std::sort(s.devices.begin(), s.devices.end(), [](const Device &a, const Device &b) {
    if (a.paired != b.paired) return a.paired;
    if (a.connected != b.connected) return a.connected;
    // unnamed devices (only an address) last
    bool an = !a.alias.empty() && a.alias != a.address, bn = !b.alias.empty() && b.alias != b.address;
    if (an != bn) return an;
    return fld::lower(a.display_name()) < fld::lower(b.display_name());
  });
  return s;
}

gboolean do_rebuild(gpointer) {
  g_rebuild_src = 0;
  State s = build_state();
  // connection changes of paired devices
  std::vector<std::pair<Device, bool>> events;
  for (auto &d : s.devices) {
    auto it = g_was_connected.find(d.path);
    if (it != g_was_connected.end() && it->second != d.connected && d.paired) events.push_back({d, d.connected});
    g_was_connected[d.path] = d.connected;
  }
  fld::ui_call([s, events] {
    Client &c = Client::get();
    // keep the UI's busy marks
    State ns = s;
    for (auto &d : ns.devices)
      if (const Device *old = c.state_.device(d.path)) d.busy = old->busy;
    c.state_ = ns;
    if (c.on_changed) c.on_changed();
    if (c.on_connection)
      for (auto &e : events) c.on_connection(e.first, e.second);
  });
  return G_SOURCE_REMOVE;
}

void schedule_rebuild() {
  if (!g_rebuild_src) g_rebuild_src = fld::glib_timeout(100, (int (*)(void *))do_rebuild, nullptr);
}

void start_discovery_if_needed();

void om_ready() {
  g_signal_connect(g_om, "object-added", G_CALLBACK(+[](GDBusObjectManager *, GDBusObject *, gpointer) { schedule_rebuild(); }), nullptr);
  g_signal_connect(g_om, "object-removed", G_CALLBACK(+[](GDBusObjectManager *, GDBusObject *, gpointer) { schedule_rebuild(); }), nullptr);
  g_signal_connect(g_om, "interface-proxy-properties-changed",
                   G_CALLBACK(+[](GDBusObjectManagerClient *, GDBusObjectProxy *, GDBusProxy *, GVariant *,
                                  const gchar *const *, gpointer) { schedule_rebuild(); }),
                   nullptr);
  g_signal_connect(g_om, "interface-added", G_CALLBACK(+[](GDBusObjectManager *, GDBusObject *, GDBusInterface *, gpointer) { schedule_rebuild(); }), nullptr);
  g_signal_connect(g_om, "interface-removed", G_CALLBACK(+[](GDBusObjectManager *, GDBusObject *, GDBusInterface *, gpointer) { schedule_rebuild(); }), nullptr);
  // the first snapshot must not report every connected device as "new"
  State s = build_state();
  for (auto &d : s.devices) g_was_connected[d.path] = d.connected;
  schedule_rebuild();
  start_discovery_if_needed();
}

void connect_bluez() {
  if (g_om) return;
  GError *err = nullptr;
  g_om = g_dbus_object_manager_client_new_sync(g_bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START,
                                               "org.bluez", "/", nullptr, nullptr, nullptr, nullptr, &err);
  if (!g_om) {
    g_printerr("bt: %s\n", err ? err->message : "no object manager");
    g_clear_error(&err);
    return;
  }
  om_ready();
}

// adapter proxy of the first adapter (new ref) or null
GDBusProxy *adapter_proxy() {
  if (!g_om) return nullptr;
  GList *objs = g_dbus_object_manager_get_objects(g_om);
  std::vector<std::pair<std::string, GDBusProxy *>> found;
  for (GList *l = objs; l; l = l->next)
    if (GDBusProxy *a = iface(G_DBUS_OBJECT(l->data), "org.bluez.Adapter1"))
      found.push_back({g_dbus_object_get_object_path(G_DBUS_OBJECT(l->data)), a});
  g_list_free_full(objs, g_object_unref);
  if (found.empty()) return nullptr;
  std::sort(found.begin(), found.end(), [](auto &a, auto &b) { return a.first < b.first; });
  for (size_t i = 1; i < found.size(); i++) g_object_unref(found[i].second);
  return found[0].second;
}

void set_prop(const std::string &path, const char *iface_name, const char *prop, GVariant *value, Done done) {
  g_variant_ref_sink(value);
  std::string p = path, i = iface_name, pr = prop;
  fld::glib_call([p, i, pr, value, done] {
    if (!g_bus) {
      g_variant_unref(value);
      finish(done, "Bluetooth is not available.");
      return;
    }
    struct Ctx {
      Done done;
    };
    g_dbus_connection_call(
        g_bus, "org.bluez", p.c_str(), "org.freedesktop.DBus.Properties", "Set",
        g_variant_new("(ssv)", i.c_str(), pr.c_str(), value), nullptr, G_DBUS_CALL_FLAGS_NONE, 10000, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
          Ctx *c = (Ctx *)data;
          GError *e = nullptr;
          GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
          if (r) g_variant_unref(r);
          finish(c->done, err_from(e));
          g_clear_error(&e);
          delete c;
        },
        new Ctx{done});
    g_variant_unref(value);
  });
}

void call(const std::string &path, const char *iface_name, const char *method, GVariant *args, int timeout_ms,
          Done done) {
  if (args) g_variant_ref_sink(args);
  std::string p = path, i = iface_name, m = method;
  fld::glib_call([p, i, m, args, timeout_ms, done] {
    if (!g_bus) {
      if (args) g_variant_unref(args);
      finish(done, "Bluetooth is not available.");
      return;
    }
    struct Ctx {
      Done done;
    };
    g_dbus_connection_call(
        g_bus, "org.bluez", p.c_str(), i.c_str(), m.c_str(), args, nullptr, G_DBUS_CALL_FLAGS_NONE, timeout_ms,
        nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
          Ctx *c = (Ctx *)data;
          GError *e = nullptr;
          GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
          if (r) g_variant_unref(r);
          finish(c->done, err_from(e));
          g_clear_error(&e);
          delete c;
        },
        new Ctx{done});
    if (args) g_variant_unref(args);
  });
}

void start_discovery_if_needed() {
  GDBusProxy *a = adapter_proxy();
  if (!a) return;
  bool want = g_discovery_users > 0 && prop_bool(a, "Powered");
  if (want && !g_discovering_by_us) {
    g_discovering_by_us = true;
    g_dbus_proxy_call(a, "StartDiscovery", nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr, nullptr);
  } else if (!want && g_discovering_by_us) {
    g_discovering_by_us = false;
    g_dbus_proxy_call(a, "StopDiscovery", nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr, nullptr);
  }
  g_object_unref(a);
}

// ---- agent ---------------------------------------------------------------------
std::function<void(const AgentRequest &, std::function<void(bool, const std::string &)>)> g_ask;

const char *kAgentXml = R"XML(
<node>
  <interface name="org.bluez.Agent1">
    <method name="Release"/>
    <method name="RequestPinCode"><arg type="o" direction="in"/><arg type="s" direction="out"/></method>
    <method name="DisplayPinCode"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
    <method name="RequestPasskey"><arg type="o" direction="in"/><arg type="u" direction="out"/></method>
    <method name="DisplayPasskey"><arg type="o" direction="in"/><arg type="u" direction="in"/><arg type="q" direction="in"/></method>
    <method name="RequestConfirmation"><arg type="o" direction="in"/><arg type="u" direction="in"/></method>
    <method name="RequestAuthorization"><arg type="o" direction="in"/></method>
    <method name="AuthorizeService"><arg type="o" direction="in"/><arg type="s" direction="in"/></method>
    <method name="Cancel"/>
  </interface>
</node>
)XML";

std::string device_name_of(const std::string &path) {
  if (!g_om) return path;
  GDBusInterface *i = g_dbus_object_manager_get_interface(g_om, path.c_str(), "org.bluez.Device1");
  if (!i) return path;
  std::string n = prop_string(G_DBUS_PROXY(i), "Alias");
  if (n.empty()) n = prop_string(G_DBUS_PROXY(i), "Address");
  g_object_unref(i);
  return n;
}

void agent_call(GDBusConnection *, const char *, const char *, const char *, const char *method, GVariant *params,
                GDBusMethodInvocation *inv, gpointer) {
  std::string m = method;
  if (m == "Release" || m == "Cancel") {
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  AgentRequest r;
  const char *dev = nullptr;
  if (m == "RequestPinCode" || m == "RequestPasskey" || m == "RequestAuthorization") {
    g_variant_get(params, "(&o)", &dev);
    r.kind = m == "RequestPinCode" ? AgentRequest::PIN : m == "RequestPasskey" ? AgentRequest::PASSKEY
                                                                               : AgentRequest::AUTHORIZE;
  } else if (m == "DisplayPinCode") {
    const char *pin;
    g_variant_get(params, "(&o&s)", &dev, &pin);
    r.kind = AgentRequest::DISPLAY_PIN;
    r.value = pin;
  } else if (m == "DisplayPasskey") {
    guint32 pk;
    guint16 entered;
    g_variant_get(params, "(&ouq)", &dev, &pk, &entered);
    r.kind = AgentRequest::DISPLAY_PASSKEY;
    r.value = format_passkey(pk);
  } else if (m == "RequestConfirmation") {
    guint32 pk;
    g_variant_get(params, "(&ou)", &dev, &pk);
    r.kind = AgentRequest::CONFIRM;
    r.value = format_passkey(pk);
  } else if (m == "AuthorizeService") {
    const char *uuid;
    g_variant_get(params, "(&o&s)", &dev, &uuid);
    r.kind = AgentRequest::AUTHORIZE_SERVICE;
    r.value = uuid;
  } else {
    g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "unknown request");
    return;
  }
  r.device_path = dev ? dev : "";
  r.device_name = device_name_of(r.device_path);
  auto ask = g_ask;
  if (!ask) {
    g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "no agent UI");
    return;
  }
  // display requests are answered at once; the dialog only informs
  bool display = r.kind == AgentRequest::DISPLAY_PIN || r.kind == AgentRequest::DISPLAY_PASSKEY;
  if (display) g_dbus_method_invocation_return_value(inv, nullptr);
  fld::ui_call([ask, r, inv, display] {
    ask(r, [r, inv, display](bool ok, const std::string &value) {
      if (display) return;
      std::string v = value;
      fld::glib_call([r, inv, ok, v] {
        if (!ok) {
          g_dbus_method_invocation_return_dbus_error(inv, "org.bluez.Error.Rejected", "Rejected by the user");
          return;
        }
        if (r.kind == AgentRequest::PIN) g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", v.c_str()));
        else if (r.kind == AgentRequest::PASSKEY)
          g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", (guint32)strtoul(v.c_str(), nullptr, 10)));
        else g_dbus_method_invocation_return_value(inv, nullptr);
      });
    });
  });
}

void register_agent() {
  if (!g_bus || !g_ask) return;
  static guint reg = 0;
  if (!reg) {
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(kAgentXml, nullptr);
    static const GDBusInterfaceVTable vt = {agent_call, nullptr, nullptr, {}};
    reg = g_dbus_connection_register_object(g_bus, kAgentPath, info->interfaces[0], &vt, nullptr, nullptr, nullptr);
  }
  g_dbus_connection_call(
      g_bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RegisterAgent",
      g_variant_new("(os)", kAgentPath, "KeyboardDisplay"), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
      [](GObject *src, GAsyncResult *res, gpointer) {
        GError *e = nullptr;
        GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &e);
        if (r) g_variant_unref(r);
        if (e && !strstr(e->message, "AlreadyExists")) g_printerr("bt: RegisterAgent: %s\n", e->message);
        g_clear_error(&e);
        g_dbus_connection_call(g_bus, "org.bluez", "/org/bluez", "org.bluez.AgentManager1", "RequestDefaultAgent",
                               g_variant_new("(o)", kAgentPath), nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
                               nullptr, nullptr);
      },
      nullptr);
}

} // namespace

Client &Client::get() {
  static Client c;
  return c;
}

void Client::start() {
  static bool started = false;
  if (started) return;
  started = true;
  if (!fld::glib_running()) fld::glib_start();
  fld::glib_call([] {
    GError *err = nullptr;
    g_bus = g_bus_get_sync(bus_type(), nullptr, &err);
    if (!g_bus) {
      g_printerr("bt: no bus: %s\n", err ? err->message : "?");
      g_clear_error(&err);
      return;
    }
    g_bus_watch_name_on_connection(
        g_bus, "org.bluez", G_BUS_NAME_WATCHER_FLAGS_NONE,
        [](GDBusConnection *, const char *, const char *, gpointer) {
          g_running = true;
          connect_bluez();
          register_agent();
          schedule_rebuild();
        },
        [](GDBusConnection *, const char *, gpointer) {
          g_running = false;
          g_discovering_by_us = false;
          if (g_om) {
            g_object_unref(g_om);
            g_om = nullptr;
          }
          schedule_rebuild();
        },
        nullptr, nullptr);
    schedule_rebuild();
    // rfkill switches have no D-Bus signal: look now and then
    fld::glib_timeout(3000, [](void *) -> int {
      static bool last_soft = false, last_hard = false;
      bool soft, hard;
      rfkill_state("/sys/class/rfkill", soft, hard);
      if (soft != last_soft || hard != last_hard) schedule_rebuild();
      last_soft = soft;
      last_hard = hard;
      return G_SOURCE_CONTINUE;
    }, nullptr);
  });
}

void Client::set_powered(bool on, Done done) {
  const Adapter *a = state_.adapter();
  if (!a) {
    if (done) done("No Bluetooth adapter.");
    return;
  }
  std::string path = a->path;
  if (on && state_.rfkill_blocked && !state_.rfkill_hard) rfkill_unblock();
  set_prop(path, "org.bluez.Adapter1", "Powered", g_variant_new_boolean(on), [path, on, done](const std::string &e) {
    if (!e.empty() && on && e.find("blocked") != std::string::npos && rfkill_unblock()) {
      // retry once after lifting the soft block
      set_prop(path, "org.bluez.Adapter1", "Powered", g_variant_new_boolean(TRUE), done);
      return;
    }
    if (done) done(e);
  });
  fld::glib_call([] { start_discovery_if_needed(); });
}

void Client::set_discoverable(bool on, Done done) {
  if (const Adapter *a = state_.adapter())
    set_prop(a->path, "org.bluez.Adapter1", "Discoverable", g_variant_new_boolean(on), done);
}

void Client::set_alias(const std::string &alias, Done done) {
  if (const Adapter *a = state_.adapter())
    set_prop(a->path, "org.bluez.Adapter1", "Alias", g_variant_new_string(alias.c_str()), done);
}

void Client::discovery(bool on) {
  fld::glib_call([on] {
    g_discovery_users = std::max(0, g_discovery_users + (on ? 1 : -1));
    start_discovery_if_needed();
  });
}

void Client::pair(const std::string &device, Done done) {
  call(device, "org.bluez.Device1", "Pair", nullptr, 90000, [device, done](const std::string &e) {
    if (e.empty()) {
      // paired devices are trusted and connected right away, as other desktops do
      Client::get().set_trusted(device, true);
      Client::get().connect(device);
    }
    if (done) done(e);
  });
}

void Client::connect(const std::string &device, Done done) {
  call(device, "org.bluez.Device1", "Connect", nullptr, 60000, done);
}

void Client::disconnect(const std::string &device, Done done) {
  call(device, "org.bluez.Device1", "Disconnect", nullptr, 20000, done);
}

void Client::set_trusted(const std::string &device, bool on, Done done) {
  set_prop(device, "org.bluez.Device1", "Trusted", g_variant_new_boolean(on), done);
}

void Client::set_blocked(const std::string &device, bool on, Done done) {
  set_prop(device, "org.bluez.Device1", "Blocked", g_variant_new_boolean(on), done);
}

void Client::remove(const std::string &device, Done done) {
  const Device *d = state_.device(device);
  std::string adapter = d ? d->adapter : (state_.adapter() ? state_.adapter()->path : "");
  if (adapter.empty()) {
    if (done) done("No Bluetooth adapter.");
    return;
  }
  call(adapter, "org.bluez.Adapter1", "RemoveDevice", g_variant_new("(o)", device.c_str()), 10000, done);
}

void Client::start_agent(
    std::function<void(const AgentRequest &, std::function<void(bool, const std::string &)>)> ask) {
  start();
  fld::glib_call([ask] {
    g_ask = ask;
    if (g_running) register_agent();
  });
}

// ---- helpers ---------------------------------------------------------------------

std::string error_text(const std::string &name, const std::string &message) {
  std::string n = name;
  if (n == "org.bluez.Error.AuthenticationFailed") return "Authentication failed. Check the PIN or passkey.";
  if (n == "org.bluez.Error.AuthenticationCanceled") return "Pairing was canceled.";
  if (n == "org.bluez.Error.AuthenticationRejected") return "The device rejected the pairing.";
  if (n == "org.bluez.Error.AuthenticationTimeout") return "The device did not answer in time.";
  if (n == "org.bluez.Error.ConnectionAttemptFailed") return "Could not connect to the device.";
  if (n == "org.bluez.Error.AlreadyExists") return "The device is already paired.";
  if (n == "org.bluez.Error.InProgress") return "The device is busy; try again.";
  if (n == "org.bluez.Error.NotReady") return "Bluetooth is turned off.";
  if (n == "org.bluez.Error.Blocked" || message.find("Blocked through rfkill") != std::string::npos)
    return "Bluetooth is blocked (rfkill or a hardware switch).";
  if (n == "org.bluez.Error.Failed" && message.find("br-connection-profile-unavailable") != std::string::npos)
    return "The device offers no profile this computer can use (is the audio service running?).";
  if (n == "org.freedesktop.DBus.Error.NoReply") return "The device did not answer in time.";
  if (n == "org.freedesktop.DBus.Error.ServiceUnknown") return "The Bluetooth service (bluetoothd) is not running.";
  return message.empty() ? n : message;
}

const char *device_icon(const std::string &icon, uint32_t cls) {
  static const std::pair<const char *, const char *> map[] = {
      {"audio-headset", "audio-headset"},     {"audio-headphones", "audio-headphones"},
      {"audio-card", "audio-speakers"},       {"phone", "phone"},
      {"computer", "computer"},               {"input-keyboard", "input-keyboard"},
      {"input-mouse", "input-mouse"},         {"input-tablet", "input-tablet"},
      {"input-gaming", "input-gaming"},       {"printer", "printer"},
      {"camera-photo", "camera-photo"},       {"camera-video", "camera-video"},
      {"modem", "modem"},                     {"network-wireless", "network-wireless"},
      {"video-display", "video-display"},     {"multimedia-player", "multimedia-player"},
  };
  for (auto &m : map)
    if (icon == m.first) return m.second;
  // major device class
  switch ((cls >> 8) & 0x1f) {
  case 1: return "computer";
  case 2: return "phone";
  case 4: return "audio-headset";
  case 5: return "input-keyboard";
  case 6: return "printer";
  }
  return "bluetooth";
}

std::string format_passkey(uint32_t pk) {
  char buf[16];
  snprintf(buf, sizeof buf, "%06u", pk % 1000000);
  return buf;
}

void rfkill_state(const std::string &root, bool &soft, bool &hard) {
  soft = hard = false;
  for (auto &n : fld::list_dir(root)) {
    std::string d = root + "/" + n, type, s, h;
    if (!fld::read_file(d + "/type", type) || fld::trim(type) != "bluetooth") continue;
    if (fld::read_file(d + "/soft", s) && fld::trim(s) == "1") soft = true;
    if (fld::read_file(d + "/hard", h) && fld::trim(h) == "1") hard = true;
  }
}

bool rfkill_unblock() {
  int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct rfkill_event ev;
  memset(&ev, 0, sizeof ev);
  ev.type = RFKILL_TYPE_BLUETOOTH;
  ev.op = RFKILL_OP_CHANGE_ALL;
  ev.soft = 0;
  bool ok = write(fd, &ev, sizeof ev) >= 0;
  close(fd);
  return ok;
}

} // namespace bt
