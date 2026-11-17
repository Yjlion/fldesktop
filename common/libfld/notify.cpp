#include "fld/notify.h"

#include <FL/Fl_Window.H>
#include <gio/gio.h>

#include <cstdlib>
#include <cstring>
#include <map>

#include "fld/glib_bridge.h"

namespace fld {

namespace {

const char *kName = "org.freedesktop.Notifications";
const char *kPath = "/org/freedesktop/Notifications";
const char *kHistIface = "org.fldesktop.Notifications";

// GLib thread only
GDBusConnection *g_bus = nullptr;
bool g_subscribed = false;
std::map<uint32_t, std::function<void(const std::string &)>> g_actions;

GDBusConnection *bus() {
  if (!g_bus) {
    GError *err = nullptr;
    g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!g_bus) {
      g_printerr("fld::notify: no session bus: %s\n", err ? err->message : "?");
      g_clear_error(&err);
    }
  }
  return g_bus;
}

void on_signal(GDBusConnection *, const char *, const char *, const char *, const char *signal,
               GVariant *params, gpointer) {
  if (!strcmp(signal, "ActionInvoked")) {
    guint32 id;
    const char *key;
    g_variant_get(params, "(u&s)", &id, &key);
    auto it = g_actions.find(id);
    if (it == g_actions.end()) return;
    auto fn = it->second;
    std::string k = key;
    ui_call([fn, k] { fn(k); });
  } else if (!strcmp(signal, "NotificationClosed")) {
    guint32 id, reason;
    g_variant_get(params, "(uu)", &id, &reason);
    g_actions.erase(id);
  }
}

void subscribe() {
  if (g_subscribed || !bus()) return;
  g_subscribed = true;
  g_dbus_connection_signal_subscribe(g_bus, nullptr, kName, nullptr, kPath, nullptr,
                                     G_DBUS_SIGNAL_FLAGS_NONE, on_signal, nullptr, nullptr);
}

std::string default_app_name() {
  const char *c = Fl_Window::default_xclass();
  return c && *c ? c : g_get_prgname() ? g_get_prgname() : "fldesktop";
}

} // namespace

void notify(const Notification &n, std::function<void(const std::string &)> on_action,
            std::function<void(uint32_t)> on_id) {
  if (!glib_running()) glib_start();
  Notification copy = n;
  if (copy.app_name.empty()) copy.app_name = default_app_name();
  glib_call([copy, on_action, on_id] {
    if (!bus()) {
      if (on_id) ui_call([on_id] { on_id(0); });
      return;
    }
    if (on_action) subscribe();
    GVariantBuilder acts;
    g_variant_builder_init(&acts, G_VARIANT_TYPE("as"));
    for (auto &a : copy.actions) {
      g_variant_builder_add(&acts, "s", a.first.c_str());
      g_variant_builder_add(&acts, "s", a.second.c_str());
    }
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte((guchar)copy.urgency));
    if (!copy.category.empty())
      g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string(copy.category.c_str()));
    if (!copy.desktop_entry.empty())
      g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(copy.desktop_entry.c_str()));
    if (copy.transient) g_variant_builder_add(&hints, "{sv}", "transient", g_variant_new_boolean(TRUE));
    GVariant *args = g_variant_new("(susssasa{sv}i)", copy.app_name.c_str(), copy.replaces_id,
                                   copy.icon.c_str(), copy.summary.c_str(), copy.body.c_str(), &acts, &hints,
                                   copy.timeout);
    struct Ctx {
      std::function<void(const std::string &)> on_action;
      std::function<void(uint32_t)> on_id;
    };
    g_dbus_connection_call(
        g_bus, kName, kPath, kName, "Notify", args, G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
          Ctx *c = (Ctx *)data;
          GError *err = nullptr;
          GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
          guint32 id = 0;
          if (r) {
            g_variant_get(r, "(u)", &id);
            g_variant_unref(r);
            if (c->on_action) g_actions[id] = c->on_action;
          } else {
            g_printerr("fld::notify: %s\n", err ? err->message : "?");
            g_clear_error(&err);
          }
          if (c->on_id) {
            auto fn = c->on_id;
            ui_call([fn, id] { fn(id); });
          }
          delete c;
        },
        new Ctx{on_action, on_id});
  });
}

void notify_close(uint32_t id) {
  if (!glib_running()) glib_start();
  glib_call([id] {
    if (!bus()) return;
    g_dbus_connection_call(g_bus, kName, kPath, kName, "CloseNotification", g_variant_new("(u)", id), nullptr,
                           G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, nullptr, nullptr);
  });
}

// ---- history client --------------------------------------------------------

NotifyHistory &NotifyHistory::get() {
  static NotifyHistory h;
  return h;
}

void NotifyHistory::start() {
  if (!glib_running()) glib_start();
  glib_call([this] {
    if (!bus()) return;
    auto changed = [](GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *,
                      gpointer data) {
      NotifyHistory *h = (NotifyHistory *)data;
      ui_call([h] {
        if (h->on_changed) h->on_changed();
      });
    };
    g_dbus_connection_signal_subscribe(g_bus, nullptr, kHistIface, "Changed", kPath, nullptr,
                                       G_DBUS_SIGNAL_FLAGS_NONE, changed, this, nullptr);
    // the daemon (re)starting or going away changes the indicator too
    g_bus_watch_name_on_connection(
        g_bus, kName, G_BUS_NAME_WATCHER_FLAGS_NONE,
        [](GDBusConnection *, const char *, const char *, gpointer data) {
          NotifyHistory *h = (NotifyHistory *)data;
          ui_call([h] {
            if (h->on_changed) h->on_changed();
          });
        },
        [](GDBusConnection *, const char *, gpointer data) {
          NotifyHistory *h = (NotifyHistory *)data;
          ui_call([h] {
            if (h->on_changed) h->on_changed();
          });
        },
        this, nullptr);
  });
}

void NotifyHistory::fetch(std::function<void(const std::vector<HistoryEntry> &, bool, bool)> cb) {
  if (!glib_running()) glib_start();
  glib_call([cb] {
    if (!bus()) {
      ui_call([cb] { cb({}, false, false); });
      return;
    }
    auto *ctx = new std::function<void(const std::vector<HistoryEntry> &, bool, bool)>(cb);
    g_dbus_connection_call(
        g_bus, kName, kPath, kHistIface, "GetHistory", nullptr, G_VARIANT_TYPE("(a(ussssx)b)"),
        G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr,
        [](GObject *src, GAsyncResult *res, gpointer data) {
          auto *fn = (std::function<void(const std::vector<HistoryEntry> &, bool, bool)> *)data;
          GError *err = nullptr;
          GVariant *r = g_dbus_connection_call_finish(G_DBUS_CONNECTION(src), res, &err);
          std::vector<HistoryEntry> list;
          gboolean dnd = FALSE;
          bool ok = r != nullptr;
          if (r) {
            GVariantIter *it;
            g_variant_get(r, "(a(ussssx)b)", &it, &dnd);
            guint32 id;
            const char *app, *summary, *body, *icon;
            gint64 t;
            while (g_variant_iter_next(it, "(u&s&s&s&sx)", &id, &app, &summary, &body, &icon, &t))
              list.push_back({id, app, summary, body, icon, t});
            g_variant_iter_free(it);
            g_variant_unref(r);
          }
          g_clear_error(&err);
          auto f = *fn;
          delete fn;
          bool d = dnd;
          ui_call([f, list, d, ok] { f(list, d, ok); });
        },
        ctx);
  });
}

static void hist_call(const char *method, GVariant *args) {
  if (!glib_running()) glib_start();
  GVariant *a = args ? g_variant_ref_sink(args) : nullptr;
  std::string m = method;
  glib_call([m, a] {
    if (bus())
      g_dbus_connection_call(g_bus, kName, kPath, kHistIface, m.c_str(), a, nullptr, G_DBUS_CALL_FLAGS_NONE, 3000,
                             nullptr, nullptr, nullptr);
    if (a) g_variant_unref(a);
  });
}

void NotifyHistory::clear() { hist_call("Clear", nullptr); }
void NotifyHistory::remove(uint32_t id) { hist_call("Remove", g_variant_new("(u)", id)); }
void NotifyHistory::set_dnd(bool on) { hist_call("SetDoNotDisturb", g_variant_new("(b)", (gboolean)on)); }

// ---- markup ---------------------------------------------------------------

std::string notify_strip_markup(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '<') {
      size_t e = s.find('>', i);
      if (e == std::string::npos) {
        out += s.substr(i);
        break;
      }
      std::string tag = s.substr(i + 1, e - i - 1);
      // a line break tag keeps the break
      if (tag == "br" || tag == "br/" || tag == "br /") out += '\n';
      i = e;
    } else if (c == '&') {
      size_t e = s.find(';', i);
      if (e != std::string::npos && e - i <= 8) {
        std::string ent = s.substr(i + 1, e - i - 1);
        std::string rep;
        if (ent == "amp") rep = "&";
        else if (ent == "lt") rep = "<";
        else if (ent == "gt") rep = ">";
        else if (ent == "quot") rep = "\"";
        else if (ent == "apos") rep = "'";
        else if (!ent.empty() && ent[0] == '#') {
          unsigned long cp = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16)
                                                                              : strtoul(ent.c_str() + 1, nullptr, 10);
          char buf[8];
          int n = cp ? g_unichar_to_utf8((gunichar)cp, buf) : 0;
          rep.assign(buf, n);
        }
        if (!rep.empty() || ent[0] == '#') {
          out += rep;
          i = e;
          continue;
        }
      }
      out += c;
    } else {
      out += c;
    }
  }
  return out;
}

bool notify_image_to_rgba(int width, int height, int rowstride, bool has_alpha, int bits, int channels,
                          const unsigned char *data, size_t len, std::vector<unsigned char> &rgba) {
  if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || bits != 8) return false;
  int ch = has_alpha ? 4 : 3;
  if (channels != ch || rowstride < width * ch) return false;
  // the last row may be shorter than rowstride
  if ((size_t)rowstride * (height - 1) + (size_t)width * ch > len) return false;
  rgba.resize((size_t)width * height * 4);
  for (int y = 0; y < height; y++) {
    const unsigned char *src = data + (size_t)y * rowstride;
    unsigned char *dst = &rgba[(size_t)y * width * 4];
    for (int x = 0; x < width; x++) {
      dst[0] = src[0];
      dst[1] = src[1];
      dst[2] = src[2];
      dst[3] = has_alpha ? src[3] : 255;
      src += ch;
      dst += 4;
    }
  }
  return true;
}

} // namespace fld
