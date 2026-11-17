#include "fwclient.h"

#include <gio/gio.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>

#include "fld/glib_bridge.h"
#include "fld/util.h"

namespace fw {

std::string ForwardPort::text() const {
  std::string t = port + "/" + protocol + " → ";
  if (!to_addr.empty()) t += to_addr + (to_port.empty() ? "" : ":" + to_port);
  else t += "port " + to_port;
  return t;
}

const Zone *State::zone(const std::string &name) const {
  for (auto &z : zones)
    if (z.name == name) return &z;
  return nullptr;
}

const Service *State::service(const std::string &name) const {
  for (auto &s : services)
    if (s.name == name) return &s;
  return nullptr;
}

namespace {

const char *kName = "org.fedoraproject.FirewallD1";
const char *kPath = "/org/fedoraproject/FirewallD1";
const char *kZoneIf = "org.fedoraproject.FirewallD1.zone";
const char *kConfigPath = "/org/fedoraproject/FirewallD1/config";
const char *kConfigIf = "org.fedoraproject.FirewallD1.config";
const char *kConfigZoneIf = "org.fedoraproject.FirewallD1.config.zone";

GDBusConnection *g_bus = nullptr;
bool g_running = false;
guint g_rebuild = 0;
std::vector<Service> g_services; // read once per daemon start

GBusType bus_type() {
  const char *b = getenv("FLD_FIREWALLD_BUS");
  return b && !strcmp(b, "session") ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
}

// synchronous call on the GLib thread (local daemon, quick)
GVariant *call_sync(const char *path, const char *iface, const char *method, GVariant *args, const char *rtype,
                    std::string *err = nullptr) {
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(g_bus, kName, path, iface, method, args,
                                            rtype ? G_VARIANT_TYPE(rtype) : nullptr,
                                            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, 30000, nullptr, &e);
  if (!r && err) {
    g_dbus_error_strip_remote_error(e);
    *err = e->message;
  }
  g_clear_error(&e);
  return r;
}

std::vector<std::string> strv(GVariant *v) {
  std::vector<std::string> out;
  if (!v) return out;
  GVariantIter it;
  g_variant_iter_init(&it, v);
  const char *s;
  while (g_variant_iter_next(&it, "&s", &s)) out.push_back(s);
  return out;
}

Zone zone_from_settings(const std::string &name, GVariant *dict) {
  Zone z;
  z.name = name;
  GVariant *v;
  const char *s;
  if (g_variant_lookup(dict, "short", "&s", &s)) z.short_name = s;
  if (g_variant_lookup(dict, "description", "&s", &s)) z.description = s;
  if (g_variant_lookup(dict, "target", "&s", &s)) z.target = s;
  if ((v = g_variant_lookup_value(dict, "services", G_VARIANT_TYPE("as")))) {
    z.services = strv(v);
    g_variant_unref(v);
  }
  if ((v = g_variant_lookup_value(dict, "interfaces", G_VARIANT_TYPE("as")))) {
    z.interfaces = strv(v);
    g_variant_unref(v);
  }
  if ((v = g_variant_lookup_value(dict, "sources", G_VARIANT_TYPE("as")))) {
    z.sources = strv(v);
    g_variant_unref(v);
  }
  if ((v = g_variant_lookup_value(dict, "ports", G_VARIANT_TYPE("a(ss)")))) {
    GVariantIter it;
    g_variant_iter_init(&it, v);
    const char *p, *proto;
    while (g_variant_iter_next(&it, "(&s&s)", &p, &proto)) z.ports.push_back({p, proto});
    g_variant_unref(v);
  }
  if ((v = g_variant_lookup_value(dict, "forward_ports", G_VARIANT_TYPE("a(ssss)")))) {
    GVariantIter it;
    g_variant_iter_init(&it, v);
    const char *p, *proto, *tp, *ta;
    while (g_variant_iter_next(&it, "(&s&s&s&s)", &p, &proto, &tp, &ta)) z.forward_ports.push_back({p, proto, tp, ta});
    g_variant_unref(v);
  }
  gboolean b;
  if (g_variant_lookup(dict, "masquerade", "b", &b)) z.masquerade = b;
  return z;
}

void load_services() {
  g_services.clear();
  GVariant *r = call_sync(kPath, kName, "listServices", nullptr, "(as)");
  if (!r) return;
  GVariant *list = g_variant_get_child_value(r, 0);
  for (auto &name : strv(list)) {
    Service s;
    s.name = name;
    GVariant *st = call_sync(kPath, kName, "getServiceSettings2", g_variant_new("(s)", name.c_str()), "(a{sv})");
    if (st) {
      GVariant *d = g_variant_get_child_value(st, 0);
      const char *x;
      if (g_variant_lookup(d, "short", "&s", &x)) s.short_name = x;
      if (g_variant_lookup(d, "description", "&s", &x)) s.description = x;
      if (GVariant *p = g_variant_lookup_value(d, "ports", G_VARIANT_TYPE("a(ss)"))) {
        GVariantIter it;
        g_variant_iter_init(&it, p);
        const char *pp, *proto;
        while (g_variant_iter_next(&it, "(&s&s)", &pp, &proto)) s.ports.push_back({pp, proto});
        g_variant_unref(p);
      }
      g_variant_unref(d);
      g_variant_unref(st);
    }
    g_services.push_back(s);
  }
  g_variant_unref(list);
  g_variant_unref(r);
}

State build() {
  State s;
  s.running = g_running;
  // network interfaces of the system
  for (auto &n : fld::list_dir("/sys/class/net"))
    if (n != "lo") s.interfaces.push_back(n);
  std::sort(s.interfaces.begin(), s.interfaces.end());
  if (!g_running) return s;
  GError *e = nullptr;
  GVariant *st = g_dbus_connection_call_sync(g_bus, kName, kPath, "org.freedesktop.DBus.Properties", "GetAll",
                                             g_variant_new("(s)", kName), G_VARIANT_TYPE("(a{sv})"),
                                             G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &e);
  g_clear_error(&e);
  if (st) {
    GVariant *d = g_variant_get_child_value(st, 0);
    const char *x;
    if (g_variant_lookup(d, "version", "&s", &x)) s.version = x;
    if (g_variant_lookup(d, "state", "&s", &x)) s.state = x;
    g_variant_unref(d);
    g_variant_unref(st);
  }
  if (GVariant *r = call_sync(kPath, kName, "getDefaultZone", nullptr, "(s)")) {
    const char *z;
    g_variant_get(r, "(&s)", &z);
    s.default_zone = z;
    g_variant_unref(r);
  }
  if (GVariant *r = call_sync(kPath, kName, "queryPanicMode", nullptr, "(b)")) {
    gboolean b;
    g_variant_get(r, "(b)", &b);
    s.panic = b;
    g_variant_unref(r);
  }
  if (GVariant *r = call_sync(kPath, kZoneIf, "getZones", nullptr, "(as)")) {
    GVariant *list = g_variant_get_child_value(r, 0);
    for (auto &name : strv(list)) {
      GVariant *zs = call_sync(kPath, kZoneIf, "getZoneSettings2", g_variant_new("(s)", name.c_str()), "(a{sv})");
      if (!zs) continue;
      GVariant *d = g_variant_get_child_value(zs, 0);
      Zone z = zone_from_settings(name, d);
      z.active = !z.interfaces.empty() || !z.sources.empty();
      s.zones.push_back(z);
      g_variant_unref(d);
      g_variant_unref(zs);
    }
    g_variant_unref(list);
    g_variant_unref(r);
  }
  std::sort(s.zones.begin(), s.zones.end(), [](const Zone &a, const Zone &b) { return a.name < b.name; });
  s.services = g_services;
  return s;
}

gboolean do_rebuild(gpointer) {
  g_rebuild = 0;
  State s = build();
  fld::ui_call([s] {
    Client &c = Client::get();
    c.state_ = s;
    if (c.on_changed) c.on_changed();
  });
  return G_SOURCE_REMOVE;
}

void schedule() {
  if (!g_rebuild) g_rebuild = fld::glib_timeout(150, (int (*)(void *))do_rebuild, nullptr);
}

void on_signal(GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *, gpointer) {
  schedule();
}

// runs `fn` (synchronous D-Bus calls) on the GLib thread and reports
void run(std::function<std::string()> fn, Done done) {
  fld::glib_call([fn, done] {
    std::string err = g_bus ? fn() : "The firewall service is not available.";
    if (!err.empty()) err = error_text(err);
    schedule();
    if (done) fld::ui_call([done, err] { done(err); });
  });
}

bool harmless(const std::string &err) {
  return err.find("ALREADY_ENABLED") != std::string::npos || err.find("NOT_ENABLED") != std::string::npos ||
         err.find("ZONE_ALREADY_SET") != std::string::npos;
}

std::string runtime_call(const char *method, GVariant *args) {
  std::string err;
  GVariant *r = call_sync(kPath, kZoneIf, method, args, nullptr, &err);
  if (r) g_variant_unref(r);
  return harmless(err) ? "" : err;
}

std::string permanent_call(const std::string &zone, const char *method, GVariant *args) {
  std::string err;
  GVariant *r = call_sync(kConfigPath, kConfigIf, "getZoneByName", g_variant_new("(s)", zone.c_str()), "(o)", &err);
  if (!r) {
    if (args) g_variant_unref(g_variant_ref_sink(args));
    return err;
  }
  const char *path;
  g_variant_get(r, "(&o)", &path);
  GVariant *r2 = call_sync(path, kConfigZoneIf, method, args, nullptr, &err);
  if (r2) g_variant_unref(r2);
  g_variant_unref(r);
  return harmless(err) ? "" : err;
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
    GError *e = nullptr;
    g_bus = g_bus_get_sync(bus_type(), nullptr, &e);
    if (!g_bus) {
      g_printerr("fw: %s\n", e ? e->message : "no bus");
      g_clear_error(&e);
      schedule();
      return;
    }
    // every firewalld signal may change what is shown
    g_dbus_connection_signal_subscribe(g_bus, kName, nullptr, nullptr, nullptr, nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
                                       on_signal, nullptr, nullptr);
    g_bus_watch_name_on_connection(
        g_bus, kName, G_BUS_NAME_WATCHER_FLAGS_NONE,
        [](GDBusConnection *, const char *, const char *, gpointer) {
          g_running = true;
          load_services();
          schedule();
        },
        [](GDBusConnection *, const char *, gpointer) {
          g_running = false;
          schedule();
        },
        nullptr, nullptr);
    schedule();
  });
}

void Client::set_default_zone(const std::string &zone, Done done) {
  run([zone] {
    std::string err;
    GVariant *r = call_sync(kPath, kName, "setDefaultZone", g_variant_new("(s)", zone.c_str()), nullptr, &err);
    if (r) g_variant_unref(r);
    return harmless(err) ? std::string() : err;
  }, done);
}

void Client::set_panic(bool on, Done done) {
  run([on] {
    std::string err;
    GVariant *r = call_sync(kPath, kName, on ? "enablePanicMode" : "disablePanicMode", nullptr, nullptr, &err);
    if (r) g_variant_unref(r);
    return harmless(err) || err.find("ALREADY") != std::string::npos ? std::string() : err;
  }, done);
}

void Client::reload(Done done) {
  run([] {
    std::string err;
    GVariant *r = call_sync(kPath, kName, "reload", nullptr, nullptr, &err);
    if (r) g_variant_unref(r);
    return err;
  }, done);
}

void Client::runtime_to_permanent(Done done) {
  run([] {
    std::string err;
    GVariant *r = call_sync(kPath, kName, "runtimeToPermanent", nullptr, nullptr, &err);
    if (r) g_variant_unref(r);
    return err;
  }, done);
}

void Client::service(const std::string &zone, const std::string &svc, bool on, bool permanent, Done done) {
  run([zone, svc, on, permanent] {
    std::string err = on ? runtime_call("addService", g_variant_new("(ssi)", zone.c_str(), svc.c_str(), 0))
                         : runtime_call("removeService", g_variant_new("(ss)", zone.c_str(), svc.c_str()));
    if (err.empty() && permanent)
      err = permanent_call(zone, on ? "addService" : "removeService", g_variant_new("(s)", svc.c_str()));
    return err;
  }, done);
}

void Client::port(const std::string &zone, const std::string &p, const std::string &proto, bool on, bool permanent,
                  Done done) {
  run([zone, p, proto, on, permanent] {
    std::string err =
        on ? runtime_call("addPort", g_variant_new("(sssi)", zone.c_str(), p.c_str(), proto.c_str(), 0))
           : runtime_call("removePort", g_variant_new("(sss)", zone.c_str(), p.c_str(), proto.c_str()));
    if (err.empty() && permanent)
      err = permanent_call(zone, on ? "addPort" : "removePort", g_variant_new("(ss)", p.c_str(), proto.c_str()));
    return err;
  }, done);
}

void Client::masquerade(const std::string &zone, bool on, bool permanent, Done done) {
  run([zone, on, permanent] {
    std::string err = on ? runtime_call("addMasquerade", g_variant_new("(si)", zone.c_str(), 0))
                         : runtime_call("removeMasquerade", g_variant_new("(s)", zone.c_str()));
    if (err.empty() && permanent) err = permanent_call(zone, on ? "addMasquerade" : "removeMasquerade", nullptr);
    return err;
  }, done);
}

void Client::source(const std::string &zone, const std::string &src, bool on, bool permanent, Done done) {
  run([zone, src, on, permanent] {
    std::string err = on ? runtime_call("addSource", g_variant_new("(ss)", zone.c_str(), src.c_str()))
                         : runtime_call("removeSource", g_variant_new("(ss)", zone.c_str(), src.c_str()));
    if (err.empty() && permanent)
      err = permanent_call(zone, on ? "addSource" : "removeSource", g_variant_new("(s)", src.c_str()));
    return err;
  }, done);
}

void Client::interface_zone(const std::string &iface, const std::string &zone, bool permanent, Done done) {
  const State st = state_;
  run([iface, zone, permanent, st] {
    std::string old;
    for (auto &z : st.zones)
      if (std::find(z.interfaces.begin(), z.interfaces.end(), iface) != z.interfaces.end()) old = z.name;
    std::string err;
    if (!zone.empty()) {
      GVariant *r = call_sync(kPath, kZoneIf, "changeZoneOfInterface",
                              g_variant_new("(ss)", zone.c_str(), iface.c_str()), nullptr, &err);
      if (r) g_variant_unref(r);
      if (harmless(err)) err.clear();
    } else if (!old.empty()) {
      err = runtime_call("removeInterface", g_variant_new("(ss)", old.c_str(), iface.c_str()));
    }
    if (err.empty() && permanent) {
      if (!old.empty() && old != zone) err = permanent_call(old, "removeInterface", g_variant_new("(s)", iface.c_str()));
      if (err.empty() && !zone.empty())
        err = permanent_call(zone, "addInterface", g_variant_new("(s)", iface.c_str()));
    }
    return err;
  }, done);
}

void Client::forward_port(const std::string &zone, const ForwardPort &f, bool on, bool permanent, Done done) {
  run([zone, f, on, permanent] {
    std::string err =
        on ? runtime_call("addForwardPort", g_variant_new("(sssssi)", zone.c_str(), f.port.c_str(), f.protocol.c_str(),
                                                          f.to_port.c_str(), f.to_addr.c_str(), 0))
           : runtime_call("removeForwardPort", g_variant_new("(sssss)", zone.c_str(), f.port.c_str(),
                                                             f.protocol.c_str(), f.to_port.c_str(), f.to_addr.c_str()));
    if (err.empty() && permanent)
      err = permanent_call(zone, on ? "addForwardPort" : "removeForwardPort",
                           g_variant_new("(ssss)", f.port.c_str(), f.protocol.c_str(), f.to_port.c_str(),
                                         f.to_addr.c_str()));
    return err;
  }, done);
}

// ---- helpers ---------------------------------------------------------------------

bool parse_port(const std::string &text, std::string &port, std::string &proto) {
  std::string t = fld::trim(text);
  size_t slash = t.find('/');
  port = fld::trim(t.substr(0, slash));
  proto = slash == std::string::npos ? "tcp" : fld::lower(fld::trim(t.substr(slash + 1)));
  if (proto != "tcp" && proto != "udp" && proto != "sctp" && proto != "dccp") return false;
  // a number or a range a-b, 1..65535
  auto num = [](const std::string &s, long &v) {
    if (s.empty() || s.size() > 5 || s.find_first_not_of("0123456789") != std::string::npos) return false;
    v = strtol(s.c_str(), nullptr, 10);
    return v >= 1 && v <= 65535;
  };
  long a, b;
  size_t dash = port.find('-');
  if (dash == std::string::npos) return num(port, a);
  return num(port.substr(0, dash), a) && num(port.substr(dash + 1), b) && a < b;
}

std::string error_text(const std::string &m) {
  if (m.find("NotAuthorized") != std::string::npos || m.find("not authorized") != std::string::npos ||
      m.find("AccessDenied") != std::string::npos)
    return "You are not allowed to change the firewall.";
  if (m.find("INVALID_PORT") != std::string::npos) return "The port is not valid.";
  if (m.find("INVALID_SERVICE") != std::string::npos) return "There is no such service.";
  if (m.find("INVALID_ZONE") != std::string::npos) return "There is no such zone.";
  if (m.find("INVALID_ADDR") != std::string::npos || m.find("INVALID_SOURCE") != std::string::npos)
    return "The address is not valid.";
  if (m.find("ServiceUnknown") != std::string::npos) return "The firewall service (firewalld) is not running.";
  return m;
}

std::string target_text(const std::string &t) {
  if (t == "default" || t.empty()) return "Reject other incoming connections";
  if (t == "ACCEPT") return "Accept all incoming connections";
  if (t == "DROP" || t == "%%DROP%%") return "Drop other incoming connections silently";
  if (t == "REJECT" || t == "%%REJECT%%") return "Reject other incoming connections";
  return t;
}

} // namespace fw
