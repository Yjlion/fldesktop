// Accent color presets shared by Settings, the greeter and the icon themes
#pragma once
#include <string>
#include <vector>

namespace fld {

struct AccentPreset {
  const char *id;    // "blue"
  const char *name;  // "Blue"
  const char *hex;   // "#3584e4"
};

// The presets, in display order. Each has an icon theme "fldesktop-<id>"
// (blue is the base theme "fldesktop").
const std::vector<AccentPreset> &accent_presets();
// Index of the preset with this color (case-insensitive "#rrggbb"), or -1.
int accent_index(const std::string &hex);
// Index of the preset closest in hue (low saturation picks graphite).
int nearest_accent(const std::string &hex);
// The icon theme matching a preset index ("fldesktop" / "fldesktop-<id>").
std::string accent_icon_theme(int index);
// Whether `theme` is one of the accent icon themes.
bool is_accent_icon_theme(const std::string &theme);

} // namespace fld
