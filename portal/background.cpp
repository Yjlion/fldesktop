// org.freedesktop.impl.portal.Background: applications that keep running
// without a window, and starting them with the session.
//
// Running in the background is allowed (fldesktop has no app tracking to
// forbid it); EnableAutostart writes ~/.config/autostart/<app id>.desktop,
// which fl-autostart starts at login like any other autostart entry.
#include <unistd.h>

#include <string>
#include <vector>

#include "fld/util.h"
#include "portal.h"

namespace portal {

void background_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  if (method == "GetAppState") {
    // no window to application tracking: nobody is known to run
    g_dbus_method_invocation_return_value(inv, g_variant_new("(@a{sv})", g_variant_new("a{sv}", nullptr)));
    return;
  }
  if (method == "NotifyBackground") {
    GVariantBuilder r;
    g_variant_builder_init(&r, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&r, "{sv}", "result", g_variant_new_uint32(1)); // allow
    reply(inv, SUCCESS, g_variant_builder_end(&r));
    return;
  }
  if (method == "EnableAutostart") {
    const char *app_id;
    gboolean enable;
    GVariant *cmd;
    guint32 flags;
    g_variant_get(params, "(&sb@asu)", &app_id, &enable, &cmd, &flags);
    std::vector<std::string> argv;
    GVariantIter it;
    g_variant_iter_init(&it, cmd);
    const char *a;
    while (g_variant_iter_next(&it, "&s", &a)) argv.push_back(a);
    g_variant_unref(cmd);
    bool ok = false;
    std::string name = autostart_file_name(app_id);
    if (!name.empty()) {
      std::string dir = fld::config_home() + "/autostart";
      std::string path = dir + "/" + name;
      if (!enable) {
        ok = unlink(path.c_str()) == 0 || !fld::file_exists(path);
      } else if (!argv.empty()) {
        fld::mkdir_p(dir, 0755);
        ok = fld::write_file(path, autostart_entry(app_id, argv, flags));
      }
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(b)", ok && enable));
    return;
  }
  g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
}

} // namespace portal
