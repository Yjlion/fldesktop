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
void screencast_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
GVariant *screencast_property(const std::string &prop);
void remotedesktop_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
GVariant *remotedesktop_property(const std::string &prop);
void inhibit_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
void background_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
void shortcuts_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv);
// org.fldesktop.GlobalShortcuts.Activate (from fl-wm, through --activate-shortcut)
void shortcuts_activate(const std::string &app_id, const std::string &id);
void print_call(const std::string &method, GVariant *params, GDBusMethodInvocation *inv, GUnixFDList *fds);

// display name of an application (dialogs.cpp)
std::string app_name(const std::string &app_id);
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
  bool high_contrast = false;
  double text_scale = 1.0;
};
Appearance read_appearance(const fld::Ini &desktop_ini);
// all known settings as namespace -> key -> value, filtered by the
// namespaces (exact names or a prefix ending in '*'; empty = all)
GVariant *settings_read_all(const Appearance &a, const std::vector<std::string> &namespaces);
// one value or nullptr
GVariant *settings_read(const Appearance &a, const std::string &ns, const std::string &key);

// ScreenCast (cast_util.cpp)
enum { CAST_MONITOR = 1, CAST_WINDOW = 2 };
enum { CAST_CURSOR_HIDDEN = 1, CAST_CURSOR_EMBEDDED = 2, CAST_CURSOR_METADATA = 4 };
struct CastSource {
  int kind = CAST_MONITOR;
  std::string id;    // output name, or the window's identifier (this session only)
  std::string title; // window title, or the screen's description
  std::string app_id;
};
// restore_data (suv) for the chosen sources, and back
GVariant *cast_restore_to_variant(const std::vector<CastSource> &chosen);
std::vector<CastSource> cast_parse_restore(GVariant *restore_data); // empty if not ours or invalid
// indices into `available` for every restored source, or empty when any is missing
std::vector<int> cast_match_restore(const std::vector<CastSource> &wanted, const std::vector<CastSource> &available);
bool cast_embed_cursor(guint32 cursor_mode);

// Background (portal_misc.cpp): ~/.config/autostart/<file> for an app id
// ("" for an id that is not a plain name) and its contents
std::string autostart_file_name(const std::string &app_id);
std::string autostart_entry(const std::string &app_id, const std::vector<std::string> &argv, unsigned flags);

// GlobalShortcuts (portal_misc.cpp): a trigger as applications write it
// ("CTRL+ALT+k", "<Control>k") to an fl-wm key combination ("Ctrl+Alt+k");
// "" when it cannot be bound
std::string trigger_to_combo(const std::string &trigger);
// ~/.config/fldesktop/global-shortcuts.ini: [app id] shortcut id = combo|description;
// fl-wm binds them and the Shortcuts page lists them
struct StoredShortcut {
  std::string id, combo, description;
};
std::string shortcuts_path();
std::vector<StoredShortcut> shortcuts_load(const std::string &path, const std::string &app_id);
void shortcuts_save(const std::string &path, const std::string &app_id, const std::vector<StoredShortcut> &list);

} // namespace portal
