// xdg-desktop-portal-fl: shared declarations
//
// D-Bus runs on the GLib thread (fld/glib_bridge.h). Interfaces that show a
// dialog hop to the FLTK thread with fld::ui_call and answer the call with
// reply() when the dialog closes.
#pragma once
#include <gio/gio.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "fld/file_dialog.h"
#include "fld/ini.h"

namespace portal {

enum Response { SUCCESS = 0, CANCELLED = 1, OTHER = 2 };

// Answers a request method: (u response, a{sv} results). results may be null
// (an empty dict). Safe from any thread.
void reply(GDBusMethodInvocation *inv, Response r, GVariant *results = nullptr);

// Exports org.freedesktop.impl.portal.Request at `handle` for the time a
// dialog is open; on Close, `close` runs in the FLTK thread.
void request_begin(const std::string &handle, std::function<void()> close);
void request_end(const std::string &handle);

GDBusConnection *connection();

// ---- interface handlers (GLib thread) --------------------------------------
void filechooser_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
void appchooser_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
void settings_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
void screenshot_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
void access_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
void account_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
void notification_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
void wallpaper_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
// starts watching desktop.ini for SettingChanged (FLTK thread)
void settings_watch();

// ---- pure helpers (portal_util.cpp, unit tested) ---------------------------

// a(sa(us)) filters and (sa(us)) current_filter
std::vector<fld::FileFilter> parse_filters(GVariant *filters);
fld::FileFilter parse_filter(GVariant *filter);
GVariant *filter_to_variant(const fld::FileFilter &f);
// a(ssa(ss)s) choices
std::vector<fld::FileChoice> parse_choices(GVariant *choices);
// ay (NUL terminated) byte strings for paths
std::string bytestring(GVariant *v);
std::string path_to_uri(const std::string &path);
// options lookup with a default
std::string opt_string(GVariant *options, const char *key, const std::string &def = "");
bool opt_bool(GVariant *options, const char *key, bool def = false);

// Settings: values derived from desktop.ini
struct Appearance {
  bool dark = false;
  double accent_r = 0.21, accent_g = 0.52, accent_b = 0.89;
  std::string icon_theme = "Adwaita", font = "Sans", mono_font = "Monospace";
  int font_size = 14, mono_size = 14; // FLTK pixels
};
Appearance read_appearance(const fld::Ini &desktop_ini);
// all known settings as namespace -> key -> value, filtered by the
// namespaces (exact names or a prefix ending in '*'; empty = all)
GVariant *settings_read_all(const Appearance &a, const std::vector<std::string> &namespaces);
// one value or nullptr
GVariant *settings_read(const Appearance &a, const std::string &ns, const std::string &key);

} // namespace portal
