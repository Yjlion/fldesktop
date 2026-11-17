// Shared look & feel for all fldesktop programs
#pragma once
#include <FL/Enumerations.H>
#include <string>

class Fl_Window;

namespace fld {

struct Palette {
  bool dark;
  Fl_Color bg, bg2, fg, fg_dim, accent, accent_fg, selection, border, panel_bg, panel_fg, danger;
};

// Reads ~/.config/fldesktop/desktop.ini [Appearance] and applies the FLTK
// scheme, colors and fonts. `app_id` becomes the Wayland app_id of all windows
// (should match the program's .desktop file name without extension).
void init_app(const char *app_id, int argc = 0, char **argv = nullptr);
const Palette &palette();
// Monospace font index (FL_COURIER mapped to the configured mono font).
Fl_Font mono_font();
int mono_size();
int ui_font_size();
std::string desktop_config_path();
// Reads the appearance from this file instead of ~/.config/fldesktop/desktop.ini
// (fl-greet: /etc/fldesktop/greeter.ini). Call before init_app().
void set_desktop_config_path(const std::string &path);
// Sets a window icon from an icon theme name.
void set_window_icon(Fl_Window *w, const char *icon_name);

} // namespace fld
