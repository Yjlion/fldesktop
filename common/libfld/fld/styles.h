// Widget styles: FLTK's built-in schemes plus fldesktop's own box drawing
#pragma once
#include <string>
#include <vector>

namespace fld {

struct WidgetStyle {
  const char *id;   // stored in desktop.ini [Appearance] Scheme=
  const char *name; // shown in Settings
  bool builtin;     // an FLTK scheme (otherwise drawn by libfld)
};

const std::vector<WidgetStyle> &widget_styles();
// Returns a known style id for `id` (case-insensitive; "base" == "none");
// unknown ids map to the default, "gtk+".
std::string normalize_style(const std::string &id);
// Applies a style to the running program and redraws all windows. `radius`
// is the corner radius of the "fld-rounded" style.
void apply_style(const std::string &id, int radius = 4);
// The style currently applied.
const std::string &current_style();

} // namespace fld
