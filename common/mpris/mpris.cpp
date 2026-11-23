#include "mpris.h"

#include <gio/gio.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>

#include "fld/glib_bridge.h"

namespace mpris {

namespace {

const char *kPrefix = "org.mpris.MediaPlayer2.";
const char *kPath = "/org/mpris/MediaPlayer2";
const char *kRoot = "org.mpris.MediaPlayer2";
const char *kPlayer = "org.mpris.MediaPlayer2.Player";

GDBusConnection *g_bus = nullptr;
guint g_rebuild_src = 0;
struct Seen {
  long long started = 0; // when it last started playing
  bool playing = false;
};
std::map<std::string, Seen> g_seen; // by bus name (the GLib thread, or command_sync)

long long now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

GVariant *get_all(GDBusConnection *c, const std::string &bus, const char *iface) {
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(c, bus.c_str(), kPath, "org.freedesktop.DBus.Properties", "GetAll",
                                            g_variant_new("(s)", iface), G_VARIANT_TYPE("(a{sv})"),
                                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 1000, nullptr, &e);
  g_clear_error(&e);
  if (!r) return nullptr;
  GVariant *d = g_variant_get_child_value(r, 0);
  g_variant_unref(r);
  return d;
}

std::string str(GVariant *d, const char *k) {
  GVariant *v = d ? g_variant_lookup_value(d, k, nullptr) : nullptr;
  std::string s;
  if (v) {
    if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(v, G_VARIANT_TYPE_OBJECT_PATH))
      s = g_variant_get_string(v, nullptr);
    else if (g_variant_is_of_type(v, G_VARIANT_TYPE_STRING_ARRAY)) {
      gsize n;
      const gchar **a = g_variant_get_strv(v, &n);
      for (gsize i = 0; i < n; i++) s += (i ? ", " : "") + std::string(a[i]);
      g_free(a);
    }
    g_variant_unref(v);
  }
  return s;
}

bool boolean(GVariant *d, const char *k) {
  GVariant *v = d ? g_variant_lookup_value(d, k, G_VARIANT_TYPE_BOOLEAN) : nullptr;
  bool b = v && g_variant_get_boolean(v);
  if (v) g_variant_unref(v);
  return b;
}

std::vector<std::string> player_names(GDBusConnection *c) {
  std::vector<std::string> names;
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                            "ListNames", nullptr, G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, 2000,
                                            nullptr, &e);
  g_clear_error(&e);
  if (!r) return names;
  GVariantIter *it;
  const char *n;
  g_variant_get(r, "(as)", &it);
  while (g_variant_iter_next(it, "&s", &n))
    if (!strncmp(n, kPrefix, strlen(kPrefix))) names.push_back(n);
  g_variant_iter_free(it);
  g_variant_unref(r);
  std::sort(names.begin(), names.end());
  return names;
}

Player read_player(GDBusConnection *c, const std::string &bus) {
  Player p;
  p.bus = bus;
  if (GVariant *d = get_all(c, bus, kRoot)) {
    p.identity = str(d, "Identity");
    p.desktop = str(d, "DesktopEntry");
    g_variant_unref(d);
  }
  if (GVariant *d = get_all(c, bus, kPlayer)) {
    p.status = str(d, "PlaybackStatus");
    p.can_play = boolean(d, "CanPlay");
    p.can_pause = boolean(d, "CanPause");
    p.can_next = boolean(d, "CanGoNext");
    p.can_prev = boolean(d, "CanGoPrevious");
    if (GVariant *m = g_variant_lookup_value(d, "Metadata", G_VARIANT_TYPE("a{sv}"))) {
      p.title = str(m, "xesam:title");
      p.artist = str(m, "xesam:artist");
      p.album = str(m, "xesam:album");
      p.art_url = str(m, "mpris:artUrl");
      g_variant_unref(m);
    }
    g_variant_unref(d);
  }
  if (p.identity.empty()) p.identity = short_name(bus);
  return p;
}

State build_state(GDBusConnection *c) {
  State s;
  std::map<std::string, Seen> seen;
  for (const std::string &n : player_names(c)) {
    Player p = read_player(c, n);
    Seen st = g_seen.count(n) ? g_seen[n] : Seen{};
    bool playing = p.status == "Playing";
    // a player that starts playing becomes the newest
    if (playing && !st.playing) st.started = now_ms();
    st.playing = playing;
    p.changed = st.started;
    seen[n] = st;
    s.players.push_back(p);
  }
  g_seen = seen; // players that left are forgotten
  return s;
}

gboolean do_rebuild(gpointer) {
  g_rebuild_src = 0;
  State s = build_state(g_bus);
  fld::ui_call([s] {
    Client &c = Client::get();
    c.state_ = s;
    if (c.on_changed) c.on_changed();
  });
  return G_SOURCE_REMOVE;
}

void schedule_rebuild() {
  if (!g_rebuild_src) g_rebuild_src = fld::glib_timeout(150, (int (*)(void *))do_rebuild, nullptr);
}

bool call(GDBusConnection *c, const std::string &bus, const std::string &method) {
  GError *e = nullptr;
  GVariant *r = g_dbus_connection_call_sync(c, bus.c_str(), kPath, kPlayer, method.c_str(), nullptr, nullptr,
                                            G_DBUS_CALL_FLAGS_NO_AUTO_START, 2000, nullptr, &e);
  g_clear_error(&e);
  if (!r) return false;
  g_variant_unref(r);
  return true;
}

} // namespace

const Player *active(const State &s) {
  const Player *best = nullptr;
  for (const Player &p : s.players) {
    if (!best) {
      best = &p;
      continue;
    }
    bool pp = p.status == "Playing", bp = best->status == "Playing";
    if (pp != bp) {
      if (pp) best = &p;
      continue;
    }
    if (p.changed > best->changed) best = &p;
  }
  return best;
}

std::string label(const Player &p) {
  if (!p.title.empty() && !p.artist.empty()) return p.title + " \xe2\x80\x93 " + p.artist;
  if (!p.title.empty()) return p.title;
  return p.identity;
}

std::string short_name(const std::string &bus) {
  std::string n = bus.compare(0, strlen(kPrefix), kPrefix) == 0 ? bus.substr(strlen(kPrefix)) : bus;
  size_t dot = n.find(".instance");
  if (dot != std::string::npos) n.resize(dot);
  return n;
}

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
    g_bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &err);
    if (!g_bus) {
      g_clear_error(&err);
      return;
    }
    // players coming and going, and their properties
    g_dbus_connection_signal_subscribe(
        g_bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *p, gpointer) {
          const char *name;
          g_variant_get(p, "(&s&s&s)", &name, nullptr, nullptr);
          if (!strncmp(name, kPrefix, strlen(kPrefix))) schedule_rebuild();
        },
        nullptr, nullptr);
    g_dbus_connection_signal_subscribe(
        g_bus, nullptr, "org.freedesktop.DBus.Properties", "PropertiesChanged", kPath, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection *, const char *, const char *, const char *, const char *, GVariant *, gpointer) {
          schedule_rebuild();
        },
        nullptr, nullptr);
    schedule_rebuild();
  });
}

void Client::command(const std::string &method) {
  const Player *p = active(state_);
  if (!p) return;
  std::string bus = p->bus;
  fld::glib_call([bus, method] {
    if (g_bus) call(g_bus, bus, method);
  });
}

bool command_sync(const std::string &method) {
  GError *e = nullptr;
  GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
  g_clear_error(&e);
  if (!c) return false;
  State s = build_state(c);
  const Player *p = active(s);
  bool ok = p && call(c, p->bus, method);
  g_object_unref(c);
  return ok;
}

} // namespace mpris
