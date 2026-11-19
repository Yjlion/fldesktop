#include "upclient.h"

#include <gio/gio.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "fld/glib_bridge.h"
#include "fld/util.h"

namespace up {

std::string Device::name() const {
  std::string n = fld::trim(vendor + " " + model);
  switch (type) {
    case 2: return "Battery" + (n.empty() ? "" : " (" + n + ")");
    case 3: return n.empty() ? "UPS" : n;
    default: break;
  }
  if (!n.empty()) return n;
  switch (type) {
    case 5: return "Mouse";
    case 6: return "Keyboard";
    case 7: return "PDA";
    case 8: return "Phone";
    case 12: return "Tablet";
    case 17: return "Headset";
    case 21: return "Headphones";
    case 23: return "Game Controller";
    case 24: return "Pen";
    default: return "Device";
  }
}

namespace {

const char *kBus = "org.freedesktop.UPower";
const char *kPath = "/org/freedesktop/UPower";
const char *kDevice = "org.freedesktop.UPower.Device";

GDBusConnection *g_bus = nullptr;
bool g_running = false;
guint g_rebuild_src = 0;
// power-profiles-daemon: the current and the older name
struct PpdName { const char *bus, *path, *iface; };
const PpdName kPpd[] = {{"org.freedesktop.UPower.PowerProfiles", "/org/freedesktop/UPower/PowerProfiles",
                         "org.freedesktop.UPower.PowerProfiles"},
                        {"net.hadess.PowerProfiles", "/net/hadess/PowerProfiles", "net.hadess.PowerProfiles"}};

GBusType bus_type() {
  const char *b = getenv("FLD_UPOWER_BUS");
  return b && !strcmp(b, "session") ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
}

// Properties.GetAll as a dictionary (new ref) or null
GVariant *get_all(const char *bus, const char *path, const char *iface) {
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(g_bus, bus, path, "org.freedesktop.DBus.Properties", "GetAll",
                                            g_variant_new("(s)", iface), G_VARIANT_TYPE("(a{sv})"),
                                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr, &e);
  g_clear_error(&e);
  if (!r) return nullptr;
  GVariant *d = g_variant_get_child_value(r, 0);
  g_variant_unref(r);
  return d;
}

template <class T> T dict(GVariant *d, const char *key, const GVariantType *type, T def, T (*get)(GVariant *)) {
  GVariant *v = d ? g_variant_lookup_value(d, key, type) : nullptr;
  if (!v) return def;
  T r = get(v);
  g_variant_unref(v);
  return r;
}
double d_double(GVariant *d, const char *k) { return dict<double>(d, k, G_VARIANT_TYPE_DOUBLE, 0, g_variant_get_double); }
bool d_bool(GVariant *d, const char *k) {
  return dict<bool>(d, k, G_VARIANT_TYPE_BOOLEAN, false, [](GVariant *v) -> bool { return g_variant_get_boolean(v); });
}
long long d_int64(GVariant *d, const char *k) {
  return dict<long long>(d, k, G_VARIANT_TYPE_INT64, 0, [](GVariant *v) -> long long { return g_variant_get_int64(v); });
}
int d_uint(GVariant *d, const char *k) {
  return dict<int>(d, k, G_VARIANT_TYPE_UINT32, 0, [](GVariant *v) -> int { return (int)g_variant_get_uint32(v); });
}
std::string d_string(GVariant *d, const char *k) {
  GVariant *v = d ? g_variant_lookup_value(d, k, G_VARIANT_TYPE_STRING) : nullptr;
  std::string s = v ? g_variant_get_string(v, nullptr) : "";
  if (v) g_variant_unref(v);
  return s;
}

Device read_device(const std::string &path) {
  Device dv;
  dv.path = path;
  GVariant *d = get_all(kBus, path.c_str(), kDevice);
  if (!d) return dv;
  dv.model = d_string(d, "Model");
  dv.vendor = d_string(d, "Vendor");
  dv.native_path = d_string(d, "NativePath");
  dv.icon = d_string(d, "IconName");
  dv.type = d_uint(d, "Type");
  dv.power_supply = d_bool(d, "PowerSupply");
  dv.present = d_bool(d, "IsPresent");
  dv.percentage = d_double(d, "Percentage");
  dv.state = d_uint(d, "State");
  dv.time_to_empty = d_int64(d, "TimeToEmpty");
  dv.time_to_full = d_int64(d, "TimeToFull");
  dv.energy = d_double(d, "Energy");
  dv.energy_full = d_double(d, "EnergyFull");
  dv.energy_full_design = d_double(d, "EnergyFullDesign");
  dv.energy_rate = d_double(d, "EnergyRate");
  dv.voltage = d_double(d, "Voltage");
  dv.capacity = d_double(d, "Capacity");
  dv.technology = "";
  switch (d_uint(d, "Technology")) {
    case 1: dv.technology = "Lithium ion"; break;
    case 2: dv.technology = "Lithium polymer"; break;
    case 3: dv.technology = "Lithium iron phosphate"; break;
    case 4: dv.technology = "Lead acid"; break;
    case 5: dv.technology = "Nickel cadmium"; break;
    case 6: dv.technology = "Nickel metal hydride"; break;
  }
  g_variant_unref(d);
  return dv;
}

State build_state() {
  State s;
  s.running = g_running && g_bus;
  if (s.running) {
    if (GVariant *d = get_all(kBus, kPath, kBus)) {
      s.on_battery = d_bool(d, "OnBattery");
      s.lid_present = d_bool(d, "LidIsPresent");
      s.lid_closed = d_bool(d, "LidIsClosed");
      g_variant_unref(d);
    }
    s.display = read_device("/org/freedesktop/UPower/devices/DisplayDevice");
    s.has_battery = s.display.present && (s.display.type == 2 || s.display.type == 3);
    GError *e = nullptr;
    GVariant *r = g_dbus_connection_call_sync(g_bus, kBus, kPath, kBus, "EnumerateDevices", nullptr,
                                              G_VARIANT_TYPE("(ao)"), G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr, &e);
    g_clear_error(&e);
    if (r) {
      GVariantIter *it;
      const char *p;
      g_variant_get(r, "(ao)", &it);
      while (g_variant_iter_next(it, "&o", &p)) {
        Device dv = read_device(p);
        if (dv.type == 1) continue; // line power
        if (dv.type == 2 && !dv.present) continue;
        s.devices.push_back(dv);
      }
      g_variant_iter_free(it);
      g_variant_unref(r);
    }
    std::sort(s.devices.begin(), s.devices.end(), [](const Device &a, const Device &b) {
      if (a.power_supply != b.power_supply) return a.power_supply;
      return a.path < b.path;
    });
  }
  for (auto &n : kPpd) {
    GVariant *d = get_all(n.bus, n.path, n.iface);
    if (!d) continue;
    s.profiles_available = true;
    s.profile = d_string(d, "ActiveProfile");
    s.degraded = d_string(d, "PerformanceDegraded");
    if (GVariant *ps = g_variant_lookup_value(d, "Profiles", G_VARIANT_TYPE("aa{sv}"))) {
      GVariantIter it;
      g_variant_iter_init(&it, ps);
      while (GVariant *p = g_variant_iter_next_value(&it)) {
        std::string name = d_string(p, "Profile");
        if (!name.empty()) s.profiles.push_back(name);
        g_variant_unref(p);
      }
      g_variant_unref(ps);
    }
    g_variant_unref(d);
    break;
  }
  return s;
}

gboolean do_rebuild(gpointer) {
  g_rebuild_src = 0;
  State s = build_state();
  fld::ui_call([s] {
    Client &c = Client::get();
    c.state_ = s;
    if (c.on_changed) c.on_changed();
  });
  return G_SOURCE_REMOVE;
}

void schedule_rebuild() {
  if (!g_rebuild_src) g_rebuild_src = fld::glib_timeout(200, (int (*)(void *))do_rebuild, nullptr);
}

void on_signal(GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *, gpointer) {
  schedule_rebuild();
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
      g_printerr("upower: no bus: %s\n", err ? err->message : "?");
      g_clear_error(&err);
      return;
    }
    // any property change of UPower objects or the profiles daemon, devices coming and going
    g_dbus_connection_signal_subscribe(g_bus, kBus, nullptr, nullptr, nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_signal, nullptr, nullptr);
    for (auto &n : kPpd)
      g_dbus_connection_signal_subscribe(g_bus, n.bus, "org.freedesktop.DBus.Properties", "PropertiesChanged", n.path,
                                         nullptr, G_DBUS_SIGNAL_FLAGS_NONE, on_signal, nullptr, nullptr);
    g_bus_watch_name_on_connection(
        g_bus, kBus, G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
        [](GDBusConnection *, const char *, const char *, gpointer) {
          g_running = true;
          schedule_rebuild();
        },
        [](GDBusConnection *, const char *, gpointer) {
          g_running = false;
          schedule_rebuild();
        },
        nullptr, nullptr);
    for (auto &n : kPpd)
      g_bus_watch_name_on_connection(
          g_bus, n.bus, G_BUS_NAME_WATCHER_FLAGS_NONE,
          [](GDBusConnection *, const char *, const char *, gpointer) { schedule_rebuild(); },
          [](GDBusConnection *, const char *, gpointer) { schedule_rebuild(); }, nullptr, nullptr);
    // time estimates change without much else: look again now and then
    fld::glib_timeout(60000, [](void *) -> int {
      schedule_rebuild();
      return G_SOURCE_CONTINUE;
    }, nullptr);
    schedule_rebuild();
  });
}

void Client::set_profile(const std::string &profile, std::function<void(const std::string &)> done) {
  std::thread([profile, done] {
    std::string err = "Power profiles are not available (install power-profiles-daemon).";
    for (auto &n : kPpd) {
      if (!g_bus) break;
      GError *e = nullptr;
      GVariant *r = g_dbus_connection_call_sync(
          g_bus, n.bus, n.path, "org.freedesktop.DBus.Properties", "Set",
          g_variant_new("(ssv)", n.iface, "ActiveProfile", g_variant_new_string(profile.c_str())), nullptr,
          G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 10000, nullptr, &e);
      if (r) {
        g_variant_unref(r);
        err.clear();
        break;
      }
      gchar *remote = e ? g_dbus_error_get_remote_error(e) : nullptr;
      bool missing = remote && (!strcmp(remote, "org.freedesktop.DBus.Error.ServiceUnknown") ||
                                !strcmp(remote, "org.freedesktop.DBus.Error.UnknownObject"));
      if (e && !missing) {
        g_dbus_error_strip_remote_error(e);
        err = e->message;
      }
      g_free(remote);
      g_clear_error(&e);
      if (!missing) break;
    }
    if (done) fld::ui_call([done, err] { done(err); });
  }).detach();
}

// ---- helpers ---------------------------------------------------------------------

std::string icon_for(double pct, int state, bool on_battery) {
  bool charging = state == CHARGING || state == FULL || state == PENDING_CHARGE || (!on_battery && state != DISCHARGING);
  std::string base = pct < 0 ? "battery-missing"
                     : pct >= 80 || state == FULL ? "battery-full"
                     : pct > 40 ? "battery-good"
                     : pct > 15 ? "battery-low"
                     : pct > 3 ? "battery-caution" : "battery-empty";
  if (base == "battery-missing" || base == "battery-empty") return base;
  return charging ? base + "-charging" : base;
}

std::string format_time(long long seconds) {
  if (seconds <= 0) return "";
  long long m = (seconds + 30) / 60;
  if (m < 60) return std::to_string(m) + " min";
  char buf[32];
  snprintf(buf, sizeof buf, "%lld h %02lld min", m / 60, m % 60);
  return buf;
}

std::string summary(const State &s) {
  if (!s.has_battery) return s.on_battery ? "On battery" : "On AC power";
  const Device &d = s.display;
  std::string pct = std::to_string((int)std::lround(d.percentage)) + " %";
  switch (d.state) {
    case FULL: return pct + ", fully charged";
    case CHARGING: {
      std::string t = format_time(d.time_to_full);
      return pct + (t.empty() ? ", charging" : ", " + t + " until full");
    }
    case DISCHARGING: {
      std::string t = format_time(d.time_to_empty);
      return pct + (t.empty() ? ", on battery" : ", " + t + " left");
    }
    case PENDING_CHARGE: return pct + ", not charging";
    default: return pct + (s.on_battery ? ", on battery" : ", plugged in");
  }
}

std::string profile_label(const std::string &p) {
  if (p == "power-saver") return "Power Saver";
  if (p == "balanced") return "Balanced";
  if (p == "performance") return "Performance";
  return p;
}

Level battery_level(double pct, bool discharging, int low, int critical, int action) {
  if (!discharging || pct < 0) return LEVEL_OK;
  if (pct <= action) return LEVEL_ACTION;
  if (pct <= critical) return LEVEL_CRITICAL;
  if (pct <= low) return LEVEL_LOW;
  return LEVEL_OK;
}

} // namespace up
