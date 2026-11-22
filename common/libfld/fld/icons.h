// freedesktop.org icon theme lookup and image loading
#pragma once
#include <FL/Enumerations.H>
#include <string>
#include <vector>

class Fl_Image;

namespace fld {

// Returns the file of the best matching icon for `name` at `size` px,
// following the configured theme, its parents, hicolor and /usr/share/pixmaps.
// `name` may also be an absolute path. Returns "" if nothing was found.
std::string icon_path(const std::string &name, int size);

// Loads (and caches) an icon scaled to size x size. The image is owned by the
// cache: never delete it. Returns nullptr if not found.
Fl_Image *icon(const std::string &name, int size);
// Loads "<name>-symbolic" (or name if it already ends so) recolored to
// `color`, falling back to the full color icon `name`. Cached; never delete.
Fl_Image *symbolic_icon(const std::string &name, int size, Fl_Color color);
// Replaces the neutral grays (and currentColor) of a symbolic SVG icon with
// `hex` ("#rrggbb"); opacities and colored parts are kept.
std::string recolor_symbolic_svg(const std::string &svg, const std::string &hex);
// Tries several names in order.
Fl_Image *icon_first(const std::vector<std::string> &names, int size);

// Loads any supported image file (png, jpg, gif, bmp, svg, xpm, ...).
// The caller owns the returned image. Returns nullptr on failure.
Fl_Image *load_image(const std::string &path);

// Name of the icon theme in use (desktop.ini [General] IconTheme, else
// default_icon_theme()).
std::string icon_theme_name();
// Uses this icon theme instead of the configured one (e.g. the greeter's).
// Call before the first icon lookup.
void set_icon_theme_name(const std::string &name);
// "fldesktop" when that theme is installed, else "Adwaita".
std::string default_icon_theme();
// "fldesktop-cursors" when installed, else "Adwaita".
std::string default_cursor_theme();

} // namespace fld
