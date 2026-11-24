// Gives GTK and Qt programs the desktop's look: light or dark, the accent
// colour, the icon theme and the font, from desktop.ini [Appearance].
//
//  GTK 3: ~/.config/gtk-3.0/settings.ini and a marked block in gtk.css
//  GTK 4: ~/.config/gtk-4.0/settings.ini (icons, font) and gtk.css
//         (libadwaita also reads the accent from the desktop portal)
//  Qt:    qt5ct / qt6ct colour schemes, ~/.config/qt{5,6}ct/colors/fldesktop.conf,
//         and qt{5,6}ct.conf when it is missing or already ours
//
// Everything outside the marked block of gtk.css, and other keys of the
// settings files, are kept. [Appearance] ToolkitColors=false turns it off.
#pragma once
#include <string>

namespace fld {
class Ini;

struct ToolkitTheme {
  bool dark = false, high_contrast = false;
  std::string accent = "#3584e4", accent_fg = "#ffffff";
  std::string window, base, text, text_dim, border, button;
  std::string icon_theme;      // "" leaves it alone
  std::string font = "Sans";
  int font_pt = 11;
};

// The theme from desktop.ini (only the values; nothing is written)
ToolkitTheme toolkit_theme_from(const Ini &desktop);

// The text between the markers; and `css` with the block replaced (or added)
std::string gtk_accent_css(const ToolkitTheme &t, int gtk_major);
std::string replace_marked_block(const std::string &css, const std::string &block);
// A qt5ct / qt6ct colour scheme file (qt 6 has one more role: Accent)
std::string qtct_color_scheme(const ToolkitTheme &t, int qt_major);

// Writes all the files under config_dir (default: $XDG_CONFIG_HOME).
// Returns false if [Appearance] ToolkitColors is off or nothing could be written.
bool export_toolkit_theme(const Ini &desktop, const std::string &config_dir = "");
// Reads desktop.ini first
bool export_toolkit_theme();

} // namespace fld
