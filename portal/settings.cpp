// org.freedesktop.impl.portal.Settings: appearance from desktop.ini
#include <memory>
#include <mutex>

#include "fld/config_watch.h"
#include "fld/glib_bridge.h"
#include "fld/theme.h"
#include "portal.h"

namespace portal {

namespace {
std::mutex g_mutex;
Appearance g_app;
bool g_loaded = false;

Appearance load() {
  fld::Ini ini;
  ini.load(fld::desktop_config_path());
  return read_appearance(ini);
}

Appearance current() {
  std::lock_guard<std::mutex> l(g_mutex);
  if (!g_loaded) {
    g_app = load();
    g_loaded = true;
  }
  return g_app;
}

const char *kKeys[][2] = {
    {"org.freedesktop.appearance", "color-scheme"},         {"org.freedesktop.appearance", "accent-color"},
    {"org.gnome.desktop.interface", "color-scheme"},        {"org.gnome.desktop.interface", "gtk-theme"},
    {"org.gnome.desktop.interface", "icon-theme"},          {"org.gnome.desktop.interface", "font-name"},
    {"org.gnome.desktop.interface", "monospace-font-name"},
    {"org.freedesktop.appearance", "contrast"},
    {"org.gnome.desktop.interface", "text-scaling-factor"},
    {"org.gnome.desktop.a11y.interface", "high-contrast"},
};

void changed() {
  Appearance old = current(), now = load();
  {
    std::lock_guard<std::mutex> l(g_mutex);
    g_app = now;
  }
  for (auto &k : kKeys) {
    GVariant *a = settings_read(old, k[0], k[1]), *b = settings_read(now, k[0], k[1]);
    g_variant_ref_sink(a);
    g_variant_ref_sink(b);
    if (!g_variant_equal(a, b)) {
      std::string ns = k[0], key = k[1];
      GVariant *v = g_variant_ref(b);
      fld::glib_call([ns, key, v] {
        if (connection())
          g_dbus_connection_emit_signal(connection(), nullptr, "/org/freedesktop/portal/desktop",
                                        "org.freedesktop.impl.portal.Settings", "SettingChanged",
                                        g_variant_new("(ssv)", ns.c_str(), key.c_str(), v), nullptr);
        g_variant_unref(v);
      });
    }
    g_variant_unref(a);
    g_variant_unref(b);
  }
}
} // namespace

void settings_watch() {
  static std::unique_ptr<fld::ConfigWatcher> w;
  current();
  w = std::make_unique<fld::ConfigWatcher>(fld::desktop_config_path(), changed);
}

void settings_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  Appearance a = current();
  if (method == "ReadAll") {
    std::vector<std::string> ns;
    GVariantIter *it;
    g_variant_get(params, "(as)", &it);
    const char *s;
    while (g_variant_iter_next(it, "&s", &s)) ns.push_back(s);
    g_variant_iter_free(it);
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sa{sv}})", settings_read_all(a, ns)));
  } else if (method == "Read") {
    const char *ns, *key;
    g_variant_get(params, "(&s&s)", &ns, &key);
    GVariant *v = settings_read(a, ns, key);
    if (!v) {
      g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.portal.Error.NotFound",
                                                 "Requested setting not found");
      return;
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
  } else {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
  }
}

} // namespace portal
