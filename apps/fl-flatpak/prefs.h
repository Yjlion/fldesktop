// fl-flatpak's Preferences: the sources (Flatpak remotes) of both
// installations, and general settings (~/.config/fldesktop/flatpak.ini).
#pragma once
#include <functional>
#include <string>

namespace fp {
struct Settings {
  std::string target = "system"; // where installs go by default: "system" / "user"
  bool refresh_on_start = false;  // download the catalogs when the program starts
};
Settings load_settings();
void save_settings(const Settings &s);
} // namespace fp

// changed(): sources were added, removed or changed (reload the catalog)
void preferences_dialog(std::function<void()> changed);
