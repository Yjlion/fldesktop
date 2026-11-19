#include "udclient.h"

#include <gio/gio.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <thread>

#include "fld/glib_bridge.h"
#include "fld/util.h"

namespace ud {

const Volume *State::volume(const std::string &path) const {
  for (auto &v : volumes)
    if (v.path == path) return &v;
  return nullptr;
}

const Volume *State::by_mount_point(const std::string &mp) const {
  for (auto &v : volumes)
    if (!mp.empty() && v.mount_point == mp) return &v;
  return nullptr;
}

namespace {

const char *kBus = "org.freedesktop.UDisks2";
const char *kBlock = "org.freedesktop.UDisks2.Block";
const char *kFs = "org.freedesktop.UDisks2.Filesystem";
const char *kEncrypted = "org.freedesktop.UDisks2.Encrypted";
const char *kDrive = "org.freedesktop.UDisks2.Drive";

// ---- GLib thread state --------------------------------------------------------
GDBusConnection *g_bus = nullptr;
GDBusObjectManager *g_om = nullptr;
bool g_running = false;
guint g_rebuild_src = 0;
bool g_first = true;              // the first snapshot announces nothing
std::set<std::string> g_known;    // volume paths seen

GBusType bus_type() {
  const char *b = getenv("FLD_UDISKS_BUS");
  return b && !strcmp(b, "session") ? G_BUS_TYPE_SESSION : G_BUS_TYPE_SYSTEM;
}

GDBusProxy *iface(GDBusObject *o, const char *name) {
  return G_DBUS_PROXY(g_dbus_object_get_interface(o, name)); // new ref or null
}

GVariant *prop(GDBusProxy *p, const char *name) { return p ? g_dbus_proxy_get_cached_property(p, name) : nullptr; }

std::string prop_string(GDBusProxy *p, const char *name) {
  GVariant *v = prop(p, name);
  std::string s;
  if (v && (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH)))
    s = g_variant_get_string(v, nullptr);
  if (v) g_variant_unref(v);
  return s == "/" ? "" : s;
}

// a byte string property (ay, nul terminated)
std::string prop_bytes(GDBusProxy *p, const char *name) {
  GVariant *v = prop(p, name);
  std::string s;
  if (v && g_variant_is_of_type(v, G_VARIANT_TYPE_BYTESTRING)) s = g_variant_get_bytestring(v);
  if (v) g_variant_unref(v);
  return s;
}

bool prop_bool(GDBusProxy *p, const char *name) {
  GVariant *v = prop(p, name);
  bool b = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
  if (v) g_variant_unref(v);
  return b;
}

unsigned long long prop_u64(GDBusProxy *p, const char *name) {
  GVariant *v = prop(p, name);
  unsigned long long n = v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64) ? g_variant_get_uint64(v) : 0;
  if (v) g_variant_unref(v);
  return n;
}

std::vector<std::string> mount_points(GDBusProxy *fs) {
  std::vector<std::string> out;
  GVariant *v = prop(fs, "MountPoints");
  if (v && g_variant_is_of_type(v, G_VARIANT_TYPE("aay"))) {
    GVariantIter it;
    g_variant_iter_init(&it, v);
    while (GVariant *c = g_variant_iter_next_value(&it)) {
      out.push_back(g_variant_get_bytestring(c));
      g_variant_unref(c);
    }
  }
  if (v) g_variant_unref(v);
  return out;
}

std::string err_from(GError *e) {
  if (!e) return "";
  gchar *remote = g_dbus_error_get_remote_error(e);
  std::string name = remote ? remote : "";
  g_free(remote);
  g_dbus_error_strip_remote_error(e);
  return error_text(name, e->message);
}

State build_state() {
  State s;
  s.running = g_running && g_om;
  if (!g_om) return s;
  GList *objs = g_dbus_object_manager_get_objects(g_om);
  // drives first: properties the volumes need
  struct DriveInfo {
    std::string name;
    bool removable = false, can_eject = false, optical = false;
  };
  std::map<std::string, DriveInfo> drives;
  for (GList *l = objs; l; l = l->next) {
    GDBusObject *o = G_DBUS_OBJECT(l->data);
    if (GDBusProxy *d = iface(o, kDrive)) {
      DriveInfo di;
      di.name = fld::trim(prop_string(d, "Vendor") + " " + prop_string(d, "Model"));
      di.removable = prop_bool(d, "Removable") || prop_bool(d, "MediaRemovable");
      di.can_eject = prop_bool(d, "Ejectable") || prop_bool(d, "CanPowerOff");
      di.optical = prop_bool(d, "Optical");
      drives[g_dbus_object_get_object_path(o)] = di;
      g_object_unref(d);
    }
  }
  std::map<std::string, std::string> cleartext_of; // encrypted block -> cleartext block
  for (GList *l = objs; l; l = l->next) {
    GDBusObject *o = G_DBUS_OBJECT(l->data);
    if (GDBusProxy *b = iface(o, kBlock)) {
      std::string backing = prop_string(b, "CryptoBackingDevice");
      if (!backing.empty()) cleartext_of[backing] = g_dbus_object_get_object_path(o);
      g_object_unref(b);
    }
  }
  for (GList *l = objs; l; l = l->next) {
    GDBusObject *o = G_DBUS_OBJECT(l->data);
    GDBusProxy *b = iface(o, kBlock);
    if (!b) continue;
    GDBusProxy *fs = iface(o, kFs);
    GDBusProxy *enc = iface(o, kEncrypted);
    std::string usage = prop_string(b, "IdUsage");
    auto mps = mount_points(fs);
    Volume v;
    v.path = g_dbus_object_get_object_path(o);
    v.device = prop_bytes(b, "PreferredDevice");
    if (v.device.empty()) v.device = prop_bytes(b, "Device");
    v.size = prop_u64(b, "Size");
    v.fs_type = prop_string(b, "IdType");
    v.read_only = prop_bool(b, "ReadOnly");
    v.hint_auto = prop_bool(b, "HintAuto");
    v.drive = prop_string(b, "Drive");
    v.backing = prop_string(b, "CryptoBackingDevice");
    v.encrypted = enc != nullptr;
    if (v.encrypted) {
      auto c = cleartext_of.find(v.path);
      if (c != cleartext_of.end()) v.cleartext = c->second;
    }
    if (!mps.empty()) v.mount_point = mps[0];
    // an unlocked volume belongs to the drive of its encrypted container
    if (v.drive.empty() && !v.backing.empty())
      if (GDBusObject *bo = g_dbus_object_manager_get_object(g_om, v.backing.c_str())) {
        if (GDBusProxy *bb = iface(bo, kBlock)) {
          v.drive = prop_string(bb, "Drive");
          g_object_unref(bb);
        }
        g_object_unref(bo);
      }
    auto di = drives.find(v.drive);
    if (di != drives.end()) {
      v.drive_name = di->second.name;
      v.removable = di->second.removable;
      v.can_eject = di->second.can_eject;
    }
    bool shown = (fs || enc) &&
                 volume_shown(usage, prop_bool(b, "HintIgnore"), prop_bool(b, "HintSystem"), v.mounted(), v.mount_point) &&
                 !(v.encrypted && !v.cleartext.empty()); // the unlocked volume is shown instead
    if (shown) {
      v.label = volume_label(prop_string(b, "HintName"), prop_string(b, "IdLabel"), v.size);
      std::string hint = prop_string(b, "HintIconName");
      bool optical = di != drives.end() && di->second.optical;
      v.icon = !hint.empty() ? hint : optical ? "media-optical" : v.removable ? "drive-removable-media" : "drive-harddisk";
      s.volumes.push_back(v);
    }
    if (fs) g_object_unref(fs);
    if (enc) g_object_unref(enc);
    g_object_unref(b);
  }
  g_list_free_full(objs, g_object_unref);
  std::sort(s.volumes.begin(), s.volumes.end(), [](const Volume &a, const Volume &b) {
    if (a.removable != b.removable) return a.removable;
    if (a.label != b.label) return fld::lower(a.label) < fld::lower(b.label);
    return a.device < b.device;
  });
  return s;
}

gboolean do_rebuild(gpointer) {
  g_rebuild_src = 0;
  State s = build_state();
  std::vector<Volume> added;
  for (auto &v : s.volumes) {
    if (!g_known.count(v.path) && !g_first) added.push_back(v);
    g_known.insert(v.path);
  }
  if (s.running) g_first = false;
  fld::ui_call([s, added] {
    Client &c = Client::get();
    c.state_ = s;
    if (c.on_changed) c.on_changed();
    if (c.on_added)
      for (auto &v : added) c.on_added(v);
  });
  return G_SOURCE_REMOVE;
}

void schedule_rebuild() {
  if (!g_rebuild_src) g_rebuild_src = fld::glib_timeout(150, (int (*)(void *))do_rebuild, nullptr);
}

void connect_udisks() {
  if (g_om) return;
  GError *err = nullptr;
  g_om = g_dbus_object_manager_client_new_sync(g_bus, G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_NONE, kBus,
                                               "/org/freedesktop/UDisks2", nullptr, nullptr, nullptr, nullptr, &err);
  if (!g_om) {
    g_printerr("udisks: %s\n", err ? err->message : "no object manager");
    g_clear_error(&err);
    return;
  }
  auto rebuild = +[](gpointer) { schedule_rebuild(); };
  g_signal_connect_swapped(g_om, "object-added", G_CALLBACK(rebuild), nullptr);
  g_signal_connect_swapped(g_om, "object-removed", G_CALLBACK(rebuild), nullptr);
  g_signal_connect_swapped(g_om, "interface-added", G_CALLBACK(rebuild), nullptr);
  g_signal_connect_swapped(g_om, "interface-removed", G_CALLBACK(rebuild), nullptr);
  g_signal_connect_swapped(g_om, "interface-proxy-properties-changed", G_CALLBACK(rebuild), nullptr);
}

// ---- calls ----------------------------------------------------------------------
// Actions run in a worker thread with synchronous calls (GDBusConnection is
// thread safe): eject is a sequence of calls, and unmounting can take long.

GVariant *options() {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
  return g_variant_builder_end(&b);
}

// returns the reply (or null and sets err)
GVariant *call_sync(const std::string &path, const char *iface_name, const char *method, GVariant *args,
                    const GVariantType *reply, std::string &err, int timeout_ms = 120000) {
  if (!g_bus) {
    if (args) g_variant_unref(g_variant_ref_sink(args));
    err = "Disks are not available (udisks2 is not running).";
    return nullptr;
  }
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(g_bus, kBus, path.c_str(), iface_name, method, args, reply,
                                            G_DBUS_CALL_FLAGS_ALLOW_INTERACTIVE_AUTHORIZATION, timeout_ms, nullptr, &e);
  if (!r) err = err_from(e);
  g_clear_error(&e);
  return r;
}

void in_worker(std::function<void()> fn) { std::thread(std::move(fn)).detach(); }

void finish(Done done, const std::string &err) {
  if (done) fld::ui_call([done, err] { done(err); });
}
void finish_value(DoneValue done, const std::string &err, const std::string &value) {
  if (done) fld::ui_call([done, err, value] { done(err, value); });
}

// the file system path for a volume: its cleartext device when unlocked
std::string fs_path(const Volume &v) { return v.encrypted && !v.cleartext.empty() ? v.cleartext : v.path; }

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
      g_printerr("udisks: no bus: %s\n", err ? err->message : "?");
      g_clear_error(&err);
      return;
    }
    g_bus_watch_name_on_connection(
        g_bus, kBus, G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
        [](GDBusConnection *, const char *, const char *, gpointer) {
          g_running = true;
          connect_udisks();
          schedule_rebuild();
        },
        [](GDBusConnection *, const char *, gpointer) {
          g_running = false;
          if (g_om) {
            g_object_unref(g_om);
            g_om = nullptr;
          }
          schedule_rebuild();
        },
        nullptr, nullptr);
  });
}

void Client::mount(const std::string &volume, DoneValue done) {
  const Volume *v = state_.volume(volume);
  std::string path = v ? fs_path(*v) : volume;
  in_worker([path, done] {
    std::string err;
    GVariant *r = call_sync(path, kFs, "Mount", g_variant_new("(@a{sv})", options()), G_VARIANT_TYPE("(s)"), err);
    std::string mp;
    if (r) {
      const char *s = nullptr;
      g_variant_get(r, "(&s)", &s);
      mp = s ? s : "";
      g_variant_unref(r);
    }
    finish_value(done, err, mp);
  });
}

void Client::unmount(const std::string &volume, Done done) {
  const Volume *v = state_.volume(volume);
  std::string path = v ? fs_path(*v) : volume;
  in_worker([path, done] {
    std::string err;
    GVariant *r = call_sync(path, kFs, "Unmount", g_variant_new("(@a{sv})", options()), nullptr, err);
    if (r) g_variant_unref(r);
    finish(done, err);
  });
}

void Client::unlock(const std::string &volume, const std::string &passphrase, DoneValue done) {
  std::string path = volume;
  in_worker([path, passphrase, done] {
    std::string err;
    GVariant *r = call_sync(path, kEncrypted, "Unlock", g_variant_new("(s@a{sv})", passphrase.c_str(), options()),
                            G_VARIANT_TYPE("(o)"), err);
    std::string clear;
    if (r) {
      const char *s = nullptr;
      g_variant_get(r, "(&o)", &s);
      clear = s ? s : "";
      g_variant_unref(r);
    }
    finish_value(done, err, clear);
  });
}

void Client::lock(const std::string &volume, Done done) {
  const Volume *v = state_.volume(volume);
  std::string path = v && !v->backing.empty() ? v->backing : volume;
  in_worker([path, done] {
    std::string err;
    GVariant *r = call_sync(path, kEncrypted, "Lock", g_variant_new("(@a{sv})", options()), nullptr, err);
    if (r) g_variant_unref(r);
    finish(done, err);
  });
}

void Client::eject(const std::string &volume, Done done) {
  const Volume *v = state_.volume(volume);
  if (!v) { finish(done, "The volume is gone."); return; }
  std::string drive = v->drive;
  // everything on the same drive: unmount, then lock
  std::vector<Volume> on_drive;
  for (auto &o : state_.volumes)
    if ((!drive.empty() && o.drive == drive) || o.path == volume) on_drive.push_back(o);
  in_worker([drive, on_drive, done] {
    std::string err;
    for (auto &o : on_drive) {
      if (o.mounted()) {
        GVariant *r = call_sync(fs_path(o), kFs, "Unmount", g_variant_new("(@a{sv})", options()), nullptr, err);
        if (!r) { finish(done, err); return; }
        g_variant_unref(r);
      }
      std::string enc = !o.backing.empty() ? o.backing : o.encrypted && !o.cleartext.empty() ? o.path : "";
      if (!enc.empty()) {
        GVariant *r = call_sync(enc, kEncrypted, "Lock", g_variant_new("(@a{sv})", options()), nullptr, err);
        if (!r) { finish(done, err); return; }
        g_variant_unref(r);
      }
    }
    if (drive.empty()) { finish(done, ""); return; }
    // eject the medium; drives without one (USB sticks) are powered off
    GVariant *r = call_sync(drive, kDrive, "Eject", g_variant_new("(@a{sv})", options()), nullptr, err);
    if (r) {
      g_variant_unref(r);
      err.clear();
    }
    std::string err2;
    r = call_sync(drive, kDrive, "PowerOff", g_variant_new("(@a{sv})", options()), nullptr, err2);
    if (r) {
      g_variant_unref(r);
      err.clear(); // powered off: safe to remove whatever Eject said
    }
    finish(done, err);
  });
}

// ---- helpers ---------------------------------------------------------------------

std::string error_text(const std::string &name, const std::string &message) {
  if (name == "org.freedesktop.UDisks2.Error.DeviceBusy")
    return "The volume is in use. Close the files and programs using it, then try again.";
  if (name == "org.freedesktop.UDisks2.Error.NotAuthorized" ||
      name == "org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain" ||
      name == "org.freedesktop.UDisks2.Error.NotAuthorizedDismissed")
    return "You are not allowed to do this.";
  if (name == "org.freedesktop.UDisks2.Error.Cancelled") return "Cancelled.";
  if (name == "org.freedesktop.UDisks2.Error.AlreadyMounted") return "The volume is already mounted.";
  if (name == "org.freedesktop.UDisks2.Error.NotMounted") return "The volume is not mounted.";
  if (name == "org.freedesktop.UDisks2.Error.Timedout") return "The device did not answer in time.";
  if (fld::starts_with(name, "org.freedesktop.UDisks2.Error.") && message.find("passphrase") != std::string::npos)
    return "Wrong passphrase.";
  if (name == "org.freedesktop.DBus.Error.ServiceUnknown" || name == "org.freedesktop.DBus.Error.NameHasNoOwner")
    return "Disks are not available (udisks2 is not running).";
  return message.empty() ? name : message;
}

std::string volume_label(const std::string &hint_name, const std::string &id_label, unsigned long long size) {
  if (!fld::trim(hint_name).empty()) return fld::trim(hint_name);
  if (!fld::trim(id_label).empty()) return fld::trim(id_label);
  if (size == 0) return "Volume";
  // like disk vendors count: 1000-based, no decimals above 10
  static const char *units[] = {"bytes", "kB", "MB", "GB", "TB", "PB"};
  double s = (double)size;
  int u = 0;
  while (s >= 1000 && u < 5) { s /= 1000; u++; }
  char buf[64];
  if (u == 0) snprintf(buf, sizeof buf, "%llu bytes Volume", size);
  else snprintf(buf, sizeof buf, s < 10 ? "%.1f %s Volume" : "%.0f %s Volume", s, units[u]);
  return buf;
}

bool volume_shown(const std::string &id_usage, bool hint_ignore, bool hint_system, bool mounted,
                  const std::string &mount_point) {
  if (id_usage != "filesystem" && id_usage != "crypto") return false;
  if (hint_ignore) return false;
  if (!hint_system) return true;
  // internal disks: only when mounted somewhere for the user
  return mounted && (fld::starts_with(mount_point, "/media/") || fld::starts_with(mount_point, "/run/media/") ||
                     fld::starts_with(mount_point, "/mnt/"));
}

} // namespace ud
