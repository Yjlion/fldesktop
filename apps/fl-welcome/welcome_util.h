// fl-welcome: what the tour writes (unit tested)
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace welcome {

// the accent colours offered (name, #rrggbb)
const std::vector<std::pair<std::string, std::string>> &accents();
// the pictures in the background folders (sorted, by file name)
std::vector<std::string> wallpapers(const std::vector<std::string> &dirs);
// ~/.config/fldesktop/welcome-done exists: the tour ran
std::string done_marker(const std::string &config_dir);
// a layout code from what the user picked ("de", "fr(bepo)") to wm.ini's
// layout and variant
void layout_code(const std::string &code, std::string &layout, std::string &variant);
// the flatpak command that adds Flathub for the user
std::vector<std::string> flathub_command();
// the guessed keyboard layout from the language ("de_DE.UTF-8" -> "de")
std::string layout_from_locale(const std::string &lang);

} // namespace welcome
