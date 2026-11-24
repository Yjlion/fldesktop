#include "secret.h"

#include <FL/Fl.H>
#include <gio/gio.h>

#include <cstring>

namespace secret {

namespace {

const char *kBus = "org.freedesktop.secrets";
const char *kPath = "/org/freedesktop/secrets";
const char *kService = "org.freedesktop.Secret.Service";
const char *kCollection = "org.freedesktop.Secret.Collection";
const char *kItem = "org.freedesktop.Secret.Item";

GDBusConnection *g_bus = nullptr;
std::string g_session; // a "plain" session for transferring secrets

std::string take_error(GError *e) {
  if (!e) return "unknown error";
  g_dbus_error_strip_remote_error(e);
  std::string m = e->message;
  g_error_free(e);
  return m;
}

GVariant *call(const std::string &path, const char *iface, const char *method, GVariant *args, const char *ret,
               std::string &err) {
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(g_bus, kBus, path.c_str(), iface, method, args,
                                            ret ? G_VARIANT_TYPE(ret) : nullptr, G_DBUS_CALL_FLAGS_NONE, 25000,
                                            nullptr, &e);
  if (!r) err = take_error(e);
  return r;
}

GVariant *get_prop(const std::string &path, const char *iface, const char *prop) {
  std::string err;
  GVariant *r = call(path, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", iface, prop), "(v)", err);
  if (!r) return nullptr;
  GVariant *v;
  g_variant_get(r, "(v)", &v);
  g_variant_unref(r);
  return v;
}

std::string prop_string(const std::string &path, const char *iface, const char *prop) {
  GVariant *v = get_prop(path, iface, prop);
  std::string s = v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) ? g_variant_get_string(v, nullptr) : "";
  if (v) g_variant_unref(v);
  return s;
}

bool prop_bool(const std::string &path, const char *iface, const char *prop) {
  GVariant *v = get_prop(path, iface, prop);
  bool b = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) && g_variant_get_boolean(v);
  if (v) g_variant_unref(v);
  return b;
}

long long prop_u64(const std::string &path, const char *iface, const char *prop) {
  GVariant *v = get_prop(path, iface, prop);
  long long n = v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT64) ? (long long)g_variant_get_uint64(v) : 0;
  if (v) g_variant_unref(v);
  return n;
}

std::vector<std::string> prop_paths(const std::string &path, const char *iface, const char *prop) {
  std::vector<std::string> out;
  GVariant *v = get_prop(path, iface, prop);
  if (v && g_variant_is_of_type(v, G_VARIANT_TYPE("ao"))) {
    GVariantIter it;
    g_variant_iter_init(&it, v);
    const char *p;
    while (g_variant_iter_next(&it, "&o", &p)) out.push_back(p);
  }
  if (v) g_variant_unref(v);
  return out;
}

// Runs a prompt the service asked for ("/" means none) and waits for it,
// keeping FLTK alive. true when it completed without being dismissed.
bool run_prompt(const std::string &prompt, std::string &err) {
  if (prompt.empty() || prompt == "/") return true;
  struct Wait {
    bool done = false, dismissed = false;
  } w;
  guint sub = g_dbus_connection_signal_subscribe(
      g_bus, kBus, "org.freedesktop.Secret.Prompt", "Completed", prompt.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
      [](GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *p, gpointer d) {
        Wait *w = (Wait *)d;
        gboolean dismissed = FALSE;
        GVariant *res = nullptr;
        g_variant_get(p, "(bv)", &dismissed, &res);
        if (res) g_variant_unref(res);
        w->dismissed = dismissed;
        w->done = true;
      },
      &w, nullptr);
  GVariant *r = call(prompt, "org.freedesktop.Secret.Prompt", "Prompt", g_variant_new("(s)", ""), nullptr, err);
  if (!r) {
    g_dbus_connection_signal_unsubscribe(g_bus, sub);
    return false;
  }
  g_variant_unref(r);
  // the signal arrives through GLib's default main context (this thread)
  while (!w.done) {
    while (g_main_context_iteration(nullptr, FALSE)) {}
    Fl::wait(0.05);
  }
  g_dbus_connection_signal_unsubscribe(g_bus, sub);
  if (w.dismissed) err = "Cancelled";
  return !w.dismissed;
}

bool ensure_session(std::string &err) {
  if (!g_session.empty()) return true;
  GVariant *r = call(kPath, kService, "OpenSession", g_variant_new("(sv)", "plain", g_variant_new_string("")),
                     "(vo)", err);
  if (!r) return false;
  GVariant *out;
  const char *path;
  g_variant_get(r, "(v&o)", &out, &path);
  g_session = path;
  g_variant_unref(out);
  g_variant_unref(r);
  return true;
}

GVariant *secret_struct(const std::string &value) {
  GVariant *params = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, "", 0, 1);
  GVariant *bytes = g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, value.data(), value.size(), 1);
  return g_variant_new("(o@ay@ays)", g_session.c_str(), params, bytes, "text/plain");
}

} // namespace

bool connect(std::string &err) {
  if (g_bus) return true;
  GError *e = nullptr;
  g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
  if (!g_bus) {
    err = take_error(e);
    return false;
  }
  // the service may be D-Bus activated (gnome-keyring-daemon)
  GVariant *r = call(kPath, "org.freedesktop.DBus.Peer", "Ping", nullptr, nullptr, err);
  if (!r) {
    err = "No password store (Secret Service) is running. Install GNOME Keyring or KeePassXC.";
    return false;
  }
  g_variant_unref(r);
  return true;
}

static Item read_item(const std::string &ip) {
  Item it;
  it.path = ip;
  it.label = prop_string(ip, kItem, "Label");
  it.locked = prop_bool(ip, kItem, "Locked");
  it.created = prop_u64(ip, kItem, "Created");
  it.modified = prop_u64(ip, kItem, "Modified");
  if (GVariant *a = get_prop(ip, kItem, "Attributes")) {
    if (g_variant_is_of_type(a, G_VARIANT_TYPE("a{ss}"))) {
      GVariantIter iter;
      g_variant_iter_init(&iter, a);
      const char *k, *v;
      while (g_variant_iter_next(&iter, "{&s&s}", &k, &v)) it.attributes.push_back({k, v});
    }
    g_variant_unref(a);
  }
  return it;
}

std::vector<Item> search(const std::vector<std::pair<std::string, std::string>> &attributes) {
  std::vector<Item> out;
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{ss}"));
  for (auto &a : attributes) g_variant_builder_add(&b, "{ss}", a.first.c_str(), a.second.c_str());
  std::string err;
  GVariant *r = call(kPath, kService, "SearchItems", g_variant_new("(a{ss})", &b), "(aoao)", err);
  if (!r) return out;
  GVariantIter *unlocked, *locked;
  g_variant_get(r, "(aoao)", &unlocked, &locked);
  const char *p;
  while (g_variant_iter_next(unlocked, "&o", &p)) out.push_back(read_item(p));
  while (g_variant_iter_next(locked, "&o", &p)) out.push_back(read_item(p));
  g_variant_iter_free(unlocked);
  g_variant_iter_free(locked);
  g_variant_unref(r);
  return out;
}

std::string default_collection() {
  std::string err;
  std::string path;
  if (GVariant *r = call(kPath, kService, "ReadAlias", g_variant_new("(s)", "default"), "(o)", err)) {
    const char *p;
    g_variant_get(r, "(&o)", &p);
    if (strcmp(p, "/") != 0) path = p;
    g_variant_unref(r);
  }
  // no default: the login collection, or the first there is
  if (path.empty()) {
    auto all = prop_paths(kPath, kService, "Collections");
    for (auto &c : all)
      if (c.find("/login") != std::string::npos) path = c;
    if (path.empty() && !all.empty()) path = all[0];
  }
  return path;
}

std::vector<Collection> collections() {
  std::vector<Collection> out;
  for (const std::string &cp : prop_paths(kPath, kService, "Collections")) {
    Collection c;
    c.path = cp;
    c.label = prop_string(cp, kCollection, "Label");
    c.locked = prop_bool(cp, kCollection, "Locked");
    for (const std::string &ip : prop_paths(cp, kCollection, "Items")) c.items.push_back(read_item(ip));
    out.push_back(c);
  }
  // which one is the default (the login keyring)
  std::string err;
  if (GVariant *r = call(kPath, kService, "ReadAlias", g_variant_new("(s)", "default"), "(o)", err)) {
    const char *p;
    g_variant_get(r, "(&o)", &p);
    for (Collection &c : out)
      if (c.path == p) c.alias = "default";
    g_variant_unref(r);
  }
  return out;
}

bool unlock(const std::string &path, std::string &err) {
  const char *paths[] = {path.c_str(), nullptr};
  GVariant *r = call(kPath, kService, "Unlock", g_variant_new("(^ao)", paths), "(aoo)", err);
  if (!r) return false;
  const char *prompt;
  g_variant_get(r, "(@ao&o)", nullptr, &prompt);
  std::string pr = prompt;
  g_variant_unref(r);
  return run_prompt(pr, err);
}

bool change_password(const std::string &collection, std::string &err) {
  // fl-secretd: Service.ChangeLock; GNOME Keyring: its own interface
  GVariant *r = call(kPath, kService, "ChangeLock", g_variant_new("(o)", collection.c_str()), "(o)", err);
  if (!r) {
    err.clear();
    r = call(kPath, "org.gnome.keyring.InternalUnsupportedGuiltRiddenInterface", "ChangeWithPrompt",
             g_variant_new("(o)", collection.c_str()), "(o)", err);
  }
  if (!r) return false;
  const char *prompt;
  g_variant_get(r, "(&o)", &prompt);
  std::string pr = prompt;
  g_variant_unref(r);
  return run_prompt(pr, err);
}

bool lock(const std::string &path, std::string &err) {
  const char *paths[] = {path.c_str(), nullptr};
  GVariant *r = call(kPath, kService, "Lock", g_variant_new("(^ao)", paths), "(aoo)", err);
  if (!r) return false;
  const char *prompt;
  g_variant_get(r, "(@ao&o)", nullptr, &prompt);
  std::string pr = prompt;
  g_variant_unref(r);
  return run_prompt(pr, err);
}

bool get_secret(const std::string &item, std::string &value, std::string &err) {
  if (!ensure_session(err)) return false;
  GVariant *r = call(item, kItem, "GetSecret", g_variant_new("(o)", g_session.c_str()), "((oayays))", err);
  if (!r) return false;
  GVariant *bytes;
  g_variant_get(r, "((o@ay@ays))", nullptr, nullptr, &bytes, nullptr);
  gsize n = 0;
  const char *d = (const char *)g_variant_get_fixed_array(bytes, &n, 1);
  value.assign(d ? d : "", n);
  g_variant_unref(bytes);
  g_variant_unref(r);
  return true;
}

bool set_secret(const std::string &item, const std::string &value, std::string &err) {
  if (!ensure_session(err)) return false;
  GVariant *r = call(item, kItem, "SetSecret", g_variant_new("(@(oayays))", secret_struct(value)), nullptr, err);
  if (!r) return false;
  g_variant_unref(r);
  return true;
}

bool set_label(const std::string &item, const std::string &label, std::string &err) {
  GVariant *r = call(item, "org.freedesktop.DBus.Properties", "Set",
                     g_variant_new("(ssv)", kItem, "Label", g_variant_new_string(label.c_str())), nullptr, err);
  if (!r) return false;
  g_variant_unref(r);
  return true;
}

bool delete_item(const std::string &item, std::string &err) {
  GVariant *r = call(item, kItem, "Delete", nullptr, "(o)", err);
  if (!r) return false;
  const char *prompt;
  g_variant_get(r, "(&o)", &prompt);
  std::string pr = prompt;
  g_variant_unref(r);
  return run_prompt(pr, err);
}

bool create_item(const std::string &collection, const std::string &label,
                 const std::vector<std::pair<std::string, std::string>> &attributes, const std::string &value,
                 std::string &err) {
  if (!ensure_session(err)) return false;
  GVariantBuilder attrs;
  g_variant_builder_init(&attrs, G_VARIANT_TYPE("a{ss}"));
  for (auto &a : attributes) g_variant_builder_add(&attrs, "{ss}", a.first.c_str(), a.second.c_str());
  GVariantBuilder props;
  g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&props, "{sv}", "org.freedesktop.Secret.Item.Label", g_variant_new_string(label.c_str()));
  g_variant_builder_add(&props, "{sv}", "org.freedesktop.Secret.Item.Attributes", g_variant_builder_end(&attrs));
  GVariant *r = call(collection, kCollection, "CreateItem",
                     g_variant_new("(@a{sv}@(oayays)b)", g_variant_builder_end(&props), secret_struct(value), FALSE),
                     "(oo)", err);
  if (!r) return false;
  const char *prompt;
  g_variant_get(r, "(&o&o)", nullptr, &prompt);
  std::string pr = prompt;
  g_variant_unref(r);
  return run_prompt(pr, err);
}

bool create_collection(const std::string &label, std::string &err) {
  GVariantBuilder props;
  g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
  g_variant_builder_add(&props, "{sv}", "org.freedesktop.Secret.Collection.Label", g_variant_new_string(label.c_str()));
  GVariant *r = call(kPath, kService, "CreateCollection", g_variant_new("(@a{sv}s)", g_variant_builder_end(&props), ""),
                     "(oo)", err);
  if (!r) return false;
  const char *prompt;
  g_variant_get(r, "(&o&o)", nullptr, &prompt);
  std::string pr = prompt;
  g_variant_unref(r);
  return run_prompt(pr, err);
}

bool delete_collection(const std::string &path, std::string &err) {
  GVariant *r = call(path, kCollection, "Delete", nullptr, "(o)", err);
  if (!r) return false;
  const char *prompt;
  g_variant_get(r, "(&o)", &prompt);
  std::string pr = prompt;
  g_variant_unref(r);
  return run_prompt(pr, err);
}

} // namespace secret
