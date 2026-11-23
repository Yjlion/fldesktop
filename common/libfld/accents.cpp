#include "fld/accents.h"
#include "fld/ini.h"
#include "fld/util.h"

#include <cmath>
#include <cstdlib>

namespace fld {

const std::vector<AccentPreset> &accent_presets() {
  static const std::vector<AccentPreset> p = {
      {"blue", "Blue", "#3584e4"},     {"teal", "Teal", "#2190a4"},   {"green", "Green", "#3a944a"},
      {"orange", "Orange", "#ed5b00"}, {"red", "Red", "#e62d42"},     {"purple", "Purple", "#9141ac"},
      {"pink", "Pink", "#d56199"},     {"graphite", "Graphite", "#6f8396"},
  };
  return p;
}

int accent_index(const std::string &hex) {
  std::string h = lower(trim(hex));
  for (size_t i = 0; i < accent_presets().size(); i++)
    if (h == accent_presets()[i].hex) return (int)i;
  return -1;
}

static bool parse_hex(const std::string &s, double &r, double &g, double &b) {
  std::string h = trim(s);
  if (h.size() != 7 || h[0] != '#') return false;
  char *end = nullptr;
  unsigned long v = strtoul(h.c_str() + 1, &end, 16);
  if (!end || *end) return false;
  r = ((v >> 16) & 255) / 255.0;
  g = ((v >> 8) & 255) / 255.0;
  b = (v & 255) / 255.0;
  return true;
}

// hue in degrees, saturation 0..1 (HSV)
static void hue_sat(double r, double g, double b, double &h, double &s) {
  double mx = std::max(r, std::max(g, b)), mn = std::min(r, std::min(g, b)), d = mx - mn;
  s = mx > 0 ? d / mx : 0;
  if (d <= 0) { h = 0; return; }
  if (mx == r) h = 60 * std::fmod((g - b) / d, 6.0);
  else if (mx == g) h = 60 * ((b - r) / d + 2);
  else h = 60 * ((r - g) / d + 4);
  if (h < 0) h += 360;
}

int nearest_accent(const std::string &hex) {
  int exact = accent_index(hex);
  if (exact >= 0) return exact;
  double r, g, b;
  if (!parse_hex(hex, r, g, b)) return 0;
  double h, s;
  hue_sat(r, g, b, h, s);
  const auto &p = accent_presets();
  int graphite = (int)p.size() - 1;
  if (s < 0.25) return graphite;
  int best = 0;
  double best_d = 1e9;
  for (int i = 0; i < graphite; i++) {
    double pr, pg, pb, ph, ps;
    parse_hex(p[i].hex, pr, pg, pb);
    hue_sat(pr, pg, pb, ph, ps);
    double d = std::fabs(h - ph);
    d = std::min(d, 360 - d);
    if (d < best_d) { best_d = d; best = i; }
  }
  return best;
}

std::string accent_icon_theme(int index) {
  const auto &p = accent_presets();
  if (index <= 0 || index >= (int)p.size()) return "fldesktop";
  return std::string("fldesktop-") + p[index].id;
}

bool is_accent_icon_theme(const std::string &theme) {
  for (size_t i = 0; i < accent_presets().size(); i++)
    if (theme == accent_icon_theme((int)i)) return true;
  return false;
}

std::string effective_icon_theme(const Ini &desktop,
                                 const std::function<bool(const std::string &)> &installed) {
  std::string stored = trim(desktop.get("General", "IconTheme", ""));
  bool follow = desktop.get_bool("Appearance", "IconsFollowAccent",
                                 stored.empty() || is_accent_icon_theme(stored));
  if (!follow || !desktop.has("Appearance", "Accent")) return stored;
  std::string want = accent_icon_theme(nearest_accent(desktop.get("Appearance", "Accent", "")));
  if (want == stored) return stored;
  if (installed && !installed(want)) return stored;
  return want;
}

} // namespace fld
