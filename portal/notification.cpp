// org.freedesktop.impl.portal.Notification: forwards to org.freedesktop.Notifications
#include <map>
#include <memory>

#include "fld/desktop_entry.h"
#include "fld/glib_bridge.h"
#include "fld/notify.h"
#include "fld/util.h"
#include "portal.h"

namespace portal {

namespace {

struct Action {
  std::string name;
  GVariant *target = nullptr; // owned
};

struct Sent {
  uint32_t server_id = 0;
};

// FLTK thread
std::map<std::string, Sent> g_sent; // app_id \n id -> server id

std::string icon_from_variant(GVariant *v) {
  if (!v) return "";
  GError *err = nullptr;
  GIcon *icon = g_icon_deserialize(v);
  std::string out;
  if (icon) {
    if (G_IS_THEMED_ICON(icon)) {
      const gchar *const *names = g_themed_icon_get_names(G_THEMED_ICON(icon));
      if (names && names[0]) out = names[0];
    } else if (G_IS_FILE_ICON(icon)) {
      gchar *p = g_file_get_path(g_file_icon_get_file(G_FILE_ICON(icon)));
      if (p) out = p;
      g_free(p);
    } else if (G_IS_BYTES_ICON(icon)) {
      GBytes *bytes = g_bytes_icon_get_bytes(G_BYTES_ICON(icon));
      gsize n = 0;
      const char *data = (const char *)g_bytes_get_data(bytes, &n);
      gchar *sum = g_compute_checksum_for_data(G_CHECKSUM_SHA1, (const guchar *)data, n);
      std::string dir = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/xdg-desktop-portal-fl-icons";
      fld::mkdir_p(dir, 0700);
      out = dir + "/" + sum + ".png";
      if (!fld::file_exists(out)) fld::write_file(out, std::string(data, n));
      g_free(sum);
    }
    g_object_unref(icon);
  }
  g_clear_error(&err);
  return out;
}

void emit_action(const std::string &app, const std::string &id, const std::string &action, GVariant *target) {
  GVariant *t = target ? g_variant_ref(target) : nullptr;
  fld::glib_call([app, id, action, t] {
    GVariantBuilder av;
    g_variant_builder_init(&av, G_VARIANT_TYPE("av"));
    if (t) g_variant_builder_add(&av, "v", t);
    if (connection())
      g_dbus_connection_emit_signal(connection(), nullptr, "/org/freedesktop/portal/desktop",
                                    "org.freedesktop.impl.portal.Notification", "ActionInvoked",
                                    g_variant_new("(sssav)", app.c_str(), id.c_str(), action.c_str(), &av), nullptr);
    if (t) g_variant_unref(t);
  });
}

} // namespace

void notification_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv) {
  if (method == "RemoveNotification") {
    const char *app, *id;
    g_variant_get(params, "(&s&s)", &app, &id);
    std::string key = std::string(app) + "\n" + id;
    fld::ui_call([key] {
      auto it = g_sent.find(key);
      if (it == g_sent.end()) return;
      if (it->second.server_id) fld::notify_close(it->second.server_id);
      g_sent.erase(it);
    });
    g_dbus_method_invocation_return_value(inv, nullptr);
    return;
  }
  if (method != "AddNotification") {
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method.c_str());
    return;
  }
  const char *app, *id;
  GVariant *n;
  g_variant_get(params, "(&s&s@a{sv})", &app, &id, &n);
  fld::Notification note;
  std::string app_id = app;
  fld::DesktopEntry e;
  if (!app_id.empty() && fld::find_application(app_id, e)) {
    note.app_name = e.name;
    note.icon = e.icon;
  } else {
    note.app_name = app_id.empty() ? "Application" : app_id;
  }
  note.desktop_entry = app_id;
  note.summary = opt_string(n, "title");
  note.body = opt_string(n, "body");
  std::string markup = opt_string(n, "markup-body");
  if (!markup.empty()) note.body = markup;
  if (GVariant *iv = g_variant_lookup_value(n, "icon", nullptr)) {
    std::string ic = icon_from_variant(iv);
    if (!ic.empty()) note.icon = ic;
    g_variant_unref(iv);
  }
  std::string prio = opt_string(n, "priority", "normal");
  note.urgency = prio == "low" ? 0 : prio == "urgent" ? 2 : 1;
  note.category = opt_string(n, "category");
  // actions: "default" and b0, b1, ... map to the application's action names
  auto actions = std::make_shared<std::map<std::string, Action>>();
  std::string def = opt_string(n, "default-action");
  if (!def.empty()) {
    note.actions.push_back({"default", "Open"});
    (*actions)["default"] = {def, g_variant_lookup_value(n, "default-action-target", nullptr)};
  }
  if (GVariant *buttons = g_variant_lookup_value(n, "buttons", G_VARIANT_TYPE("aa{sv}"))) {
    GVariantIter it;
    g_variant_iter_init(&it, buttons);
    GVariant *b;
    int i = 0;
    while ((b = g_variant_iter_next_value(&it))) {
      std::string label = opt_string(b, "label"), action = opt_string(b, "action");
      if (!label.empty() && !action.empty()) {
        std::string key = "b" + std::to_string(i++);
        note.actions.push_back({key, label});
        (*actions)[key] = {action, g_variant_lookup_value(b, "target", nullptr)};
      }
      g_variant_unref(b);
    }
    g_variant_unref(buttons);
  }
  g_variant_unref(n);
  g_dbus_method_invocation_return_value(inv, nullptr);

  std::string key = app_id + "\n" + id, nid = id;
  fld::ui_call([note, key, app_id, nid, actions]() mutable {
    auto it = g_sent.find(key);
    if (it != g_sent.end()) note.replaces_id = it->second.server_id;
    fld::notify(
        note,
        [app_id, nid, actions](const std::string &k) {
          auto a = actions->find(k);
          if (a != actions->end()) emit_action(app_id, nid, a->second.name, a->second.target);
        },
        [key](uint32_t sid) { g_sent[key].server_id = sid; });
  });
}

} // namespace portal
