#include "mpris_server.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <unistd.h>

#include <cmath>
#include <mutex>

#include "fld/glib_bridge.h"

namespace mprisd {

namespace {

const char *kPath = "/org/mpris/MediaPlayer2";
const char *kRoot = "org.mpris.MediaPlayer2";
const char *kPlayer = "org.mpris.MediaPlayer2.Player";

const char *kXml = R"XML(
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Play"/>
    <method name="Seek"><arg direction="in" name="Offset" type="x"/></method>
    <method name="SetPosition">
      <arg direction="in" name="TrackId" type="o"/><arg direction="in" name="Position" type="x"/>
    </method>
    <method name="OpenUri"><arg direction="in" name="Uri" type="s"/></method>
    <signal name="Seeked"><arg name="Position" type="x"/></signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="LoopStatus" type="s" access="readwrite"/>
    <property name="Rate" type="d" access="readwrite"/>
    <property name="Shuffle" type="b" access="readwrite"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>
)XML";

std::mutex g_mutex;
Status g_status; // guarded
Handlers g_h;    // set once
GDBusConnection *g_conn = nullptr;

gint64 us(double s) { return s > 0 ? (gint64)std::llround(s * 1e6) : 0; }

GVariant *metadata(const Status &s) {
  GVariantBuilder b;
  g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
  std::string id = s.track_id.empty() ? "/org/mpris/MediaPlayer2/TrackList/NoTrack" : s.track_id;
  g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path(id.c_str()));
  if (s.length > 0) g_variant_builder_add(&b, "{sv}", "mpris:length", g_variant_new_int64(us(s.length)));
  if (!s.title.empty()) g_variant_builder_add(&b, "{sv}", "xesam:title", g_variant_new_string(s.title.c_str()));
  if (!s.artist.empty()) {
    const gchar *a[] = {s.artist.c_str(), nullptr};
    g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_new_strv(a, 1));
  }
  if (!s.album.empty()) g_variant_builder_add(&b, "{sv}", "xesam:album", g_variant_new_string(s.album.c_str()));
  if (!s.art_url.empty()) g_variant_builder_add(&b, "{sv}", "mpris:artUrl", g_variant_new_string(s.art_url.c_str()));
  if (!s.url.empty()) g_variant_builder_add(&b, "{sv}", "xesam:url", g_variant_new_string(s.url.c_str()));
  return g_variant_builder_end(&b);
}

GVariant *property(const std::string &iface, const std::string &p, const Status &s) {
  if (iface == kRoot) {
    if (p == "CanQuit" || p == "CanRaise") return g_variant_new_boolean(TRUE);
    if (p == "HasTrackList") return g_variant_new_boolean(FALSE);
    if (p == "Identity") return g_variant_new_string("Media Player");
    if (p == "DesktopEntry") return g_variant_new_string("fl-media-player");
    if (p == "SupportedUriSchemes") {
      const gchar *v[] = {"file", "http", "https", "rtsp", "mms", nullptr};
      return g_variant_new_strv(v, -1);
    }
    if (p == "SupportedMimeTypes") {
      const gchar *v[] = {"audio/mpeg", "audio/ogg", "audio/flac", "audio/x-wav", "video/mp4", "video/webm",
                          "video/x-matroska", nullptr};
      return g_variant_new_strv(v, -1);
    }
    return nullptr;
  }
  if (p == "PlaybackStatus") return g_variant_new_string(s.playback.c_str());
  if (p == "LoopStatus") return g_variant_new_string(s.loop.c_str());
  if (p == "Rate") return g_variant_new_double(s.rate);
  if (p == "Shuffle") return g_variant_new_boolean(s.shuffle);
  if (p == "Metadata") return metadata(s);
  if (p == "Volume") return g_variant_new_double(s.volume);
  if (p == "Position") return g_variant_new_int64(us(s.position));
  if (p == "MinimumRate") return g_variant_new_double(0.25);
  if (p == "MaximumRate") return g_variant_new_double(4.0);
  if (p == "CanGoNext") return g_variant_new_boolean(s.can_next);
  if (p == "CanGoPrevious") return g_variant_new_boolean(s.can_prev);
  if (p == "CanPlay" || p == "CanPause") return g_variant_new_boolean(!s.track_id.empty());
  if (p == "CanSeek") return g_variant_new_boolean(s.can_seek);
  if (p == "CanControl") return g_variant_new_boolean(TRUE);
  return nullptr;
}

void on_call(GDBusConnection *, const char *, const char *, const char *iface, const char *method, GVariant *params,
             GDBusMethodInvocation *inv, gpointer) {
  std::string m = method;
  if (std::string(iface) == kRoot) {
    if (m == "Raise" && g_h.raise) fld::ui_call(g_h.raise);
    if (m == "Quit" && g_h.quit) fld::ui_call(g_h.quit);
  } else if (m == "Seek") {
    gint64 off;
    g_variant_get(params, "(x)", &off);
    if (g_h.seek) fld::ui_call([off] { g_h.seek(off / 1e6); });
  } else if (m == "SetPosition") {
    const char *id;
    gint64 pos;
    g_variant_get(params, "(&ox)", &id, &pos);
    std::string track = id;
    bool same;
    {
      std::lock_guard<std::mutex> l(g_mutex);
      same = track == g_status.track_id;
    }
    if (same && g_h.set_position) fld::ui_call([pos] { g_h.set_position(pos / 1e6); });
  } else if (m == "OpenUri") {
    const char *u;
    g_variant_get(params, "(&s)", &u);
    std::string uri = u;
    if (g_h.open) fld::ui_call([uri] { g_h.open(uri); });
  } else if (g_h.command) {
    fld::ui_call([m] { g_h.command(m); });
  }
  g_dbus_method_invocation_return_value(inv, nullptr);
}

GVariant *on_get(GDBusConnection *, const char *, const char *, const char *iface, const char *prop, GError **,
                 gpointer) {
  std::lock_guard<std::mutex> l(g_mutex);
  return property(iface, prop, g_status);
}

gboolean on_set(GDBusConnection *, const char *, const char *, const char *, const char *prop, GVariant *v, GError **,
                gpointer) {
  std::string p = prop;
  if (p == "Volume" && g_h.set_volume) {
    double d = g_variant_get_double(v);
    fld::ui_call([d] { g_h.set_volume(std::max(0.0, std::min(1.3, d))); });
  } else if (p == "Shuffle" && g_h.set_shuffle) {
    bool b = g_variant_get_boolean(v);
    fld::ui_call([b] { g_h.set_shuffle(b); });
  } else if (p == "LoopStatus" && g_h.set_loop) {
    std::string s = g_variant_get_string(v, nullptr);
    fld::ui_call([s] { g_h.set_loop(s); });
  }
  return TRUE;
}

} // namespace

void start(const Handlers &h) {
  g_h = h;
  if (!fld::glib_running()) fld::glib_start();
  fld::glib_call([] {
    GError *e = nullptr;
    g_conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &e);
    if (!g_conn) {
      g_clear_error(&e);
      return;
    }
    GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(kXml, nullptr);
    static const GDBusInterfaceVTable vt = {on_call, on_get, on_set, {}};
    for (int i = 0; info->interfaces[i]; i++)
      g_dbus_connection_register_object(g_conn, kPath, info->interfaces[i], &vt, nullptr, nullptr, nullptr);
    std::string name = "org.mpris.MediaPlayer2.fl_media_player.instance" + std::to_string(getpid());
    g_bus_own_name_on_connection(g_conn, name.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr,
                                 nullptr);
  });
}

void update(const Status &s) {
  Status old;
  {
    std::lock_guard<std::mutex> l(g_mutex);
    old = g_status;
    g_status = s;
  }
  fld::glib_call([old, s] {
    if (!g_conn) return;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    bool any = false;
    auto add = [&](const char *p, bool changed) {
      if (!changed) return;
      g_variant_builder_add(&b, "{sv}", p, property(kPlayer, p, s));
      any = true;
    };
    add("PlaybackStatus", old.playback != s.playback);
    add("LoopStatus", old.loop != s.loop);
    add("Shuffle", old.shuffle != s.shuffle);
    add("Rate", old.rate != s.rate);
    add("Volume", std::fabs(old.volume - s.volume) > 1e-4);
    add("Metadata", old.track_id != s.track_id || old.title != s.title || old.artist != s.artist ||
                        old.album != s.album || old.art_url != s.art_url || std::fabs(old.length - s.length) > 0.5);
    add("CanGoNext", old.can_next != s.can_next);
    add("CanGoPrevious", old.can_prev != s.can_prev);
    add("CanSeek", old.can_seek != s.can_seek);
    if (!any) {
      g_variant_builder_clear(&b);
      return;
    }
    g_dbus_connection_emit_signal(g_conn, nullptr, kPath, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                  g_variant_new("(sa{sv}as)", kPlayer, &b, nullptr), nullptr);
  });
}

void inhibit_idle(bool on) {
  static int fd = -1; // -1 none, -2 asked, else the lock
  if (!on) {
    if (fd >= 0) close(fd);
    fd = -1; // an answer still on its way is closed when it comes
    return;
  }
  if (fd != -1) return;
  fd = -2;
  fld::glib_call([] {
    GError *e = nullptr;
    GDBusConnection *sys = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &e);
    g_clear_error(&e);
    int got = -1;
    if (sys) {
      GUnixFDList *out = nullptr;
      GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(
          sys, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", "Inhibit",
          g_variant_new("(ssss)", "idle", "Media Player", "Playing music", "block"), G_VARIANT_TYPE("(h)"),
          G_DBUS_CALL_FLAGS_NO_AUTO_START, 3000, nullptr, &out, nullptr, &e);
      g_clear_error(&e);
      if (r) {
        gint32 idx;
        g_variant_get(r, "(h)", &idx);
        got = g_unix_fd_list_get(out, idx, nullptr);
        g_variant_unref(r);
      }
      if (out) g_object_unref(out);
      g_object_unref(sys);
    }
    fld::ui_call([got] {
      if (fd == -2) fd = got >= 0 ? got : -1;
      else if (got >= 0) close(got); // stopped meanwhile
    });
  });
}

void seeked(double seconds) {
  fld::glib_call([seconds] {
    if (g_conn)
      g_dbus_connection_emit_signal(g_conn, nullptr, kPath, kPlayer, "Seeked", g_variant_new("(x)", us(seconds)), nullptr);
  });
}

} // namespace mprisd
