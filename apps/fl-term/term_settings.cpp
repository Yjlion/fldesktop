#include "term_settings.h"

#include <algorithm>
#include "fld/ini.h"
#include "fld/util.h"

namespace term {

static const char *G = "Terminal";

const char *tabs_mode_name(TabsMode m) {
  switch (m) {
    case TABS_ALWAYS: return "always";
    case TABS_NEVER: return "never";
    default: return "auto";
  }
}

TabsMode parse_tabs_mode(const std::string &s) {
  std::string l = fld::lower(fld::trim(s));
  if (l == "always" || l == "true" || l == "yes" || l == "1") return TABS_ALWAYS;
  if (l == "never" || l == "false" || l == "no" || l == "0") return TABS_NEVER;
  return TABS_AUTO;
}

double clamp_opacity(double o) {
  if (!(o == o)) return 1.0; // NaN
  return std::max(0.2, std::min(1.0, o));
}

bool tabs_visible(TabsMode m, int ntabs) {
  if (m == TABS_ALWAYS) return true;
  if (m == TABS_NEVER) return false;
  return ntabs > 1;
}

void load(const fld::Ini &ini, Settings &s) {
  s.scheme = ini.get(G, "ColorScheme", s.scheme);
  s.font_size = std::max(0, ini.get_int(G, "FontSize", s.font_size));
  s.scrollback = std::max(0, ini.get_int(G, "Scrollback", s.scrollback));
  // Opacity accepts 0..1 or a percentage
  double o = ini.get_double(G, "Opacity", s.opacity);
  if (o > 1.0) o /= 100.0;
  s.opacity = clamp_opacity(o);
  s.show_menubar = ini.get_bool(G, "ShowMenuBar", s.show_menubar);
  s.tabs = parse_tabs_mode(ini.get(G, "ShowTabs", tabs_mode_name(s.tabs)));
  s.default_profile = ini.get(G, "DefaultProfile", s.default_profile);
}

std::vector<Profile> load_profiles(const fld::Ini &ini) {
  std::vector<Profile> out;
  for (auto &g : ini.groups()) {
    if (g.compare(0, 8, "Profile ") != 0 || g.size() == 8) continue;
    Profile p;
    p.name = g.substr(8);
    p.command = ini.get(g, "Command", "");
    p.cwd = ini.get(g, "WorkingDirectory", "");
    p.scheme = ini.get(g, "ColorScheme", "");
    p.font_size = std::max(0, ini.get_int(g, "FontSize", 0));
    double o = ini.get_double(g, "Opacity", 0);
    if (o > 1.0) o /= 100.0;
    p.opacity = o > 0 ? clamp_opacity(o) : 0;
    out.push_back(p);
  }
  return out;
}

void store_profiles(const std::vector<Profile> &ps, fld::Ini &ini) {
  for (auto &g : ini.groups())
    if (g.compare(0, 8, "Profile ") == 0) ini.remove_group(g);
  for (auto &p : ps) {
    std::string g = "Profile " + p.name;
    ini.set(g, "Command", p.command);
    ini.set(g, "WorkingDirectory", p.cwd);
    ini.set(g, "ColorScheme", p.scheme);
    ini.set_int(g, "FontSize", p.font_size);
    char buf[32];
    snprintf(buf, sizeof buf, "%.2f", p.opacity);
    ini.set(g, "Opacity", buf);
  }
}

const Profile *find_profile(const std::vector<Profile> &ps, const std::string &name) {
  for (auto &p : ps)
    if (p.name == name) return &p;
  return nullptr;
}

void store(const Settings &s, fld::Ini &ini) {
  ini.set(G, "ColorScheme", s.scheme);
  ini.set_int(G, "FontSize", s.font_size);
  ini.set_int(G, "Scrollback", s.scrollback);
  char buf[32];
  snprintf(buf, sizeof buf, "%.2f", clamp_opacity(s.opacity));
  ini.set(G, "Opacity", buf);
  ini.set_bool(G, "ShowMenuBar", s.show_menubar);
  ini.set(G, "ShowTabs", tabs_mode_name(s.tabs));
  ini.set(G, "DefaultProfile", s.default_profile);
}

std::string config_path() { return fld::fld_config_dir() + "/term.ini"; }

Settings load_file() {
  Settings s;
  fld::Ini ini;
  ini.load(config_path());
  load(ini, s);
  return s;
}

bool save_file(const Settings &s) {
  fld::Ini ini;
  ini.load(config_path());
  store(s, ini);
  return ini.save(config_path());
}

} // namespace term
