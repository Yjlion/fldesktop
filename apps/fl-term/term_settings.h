// fl-term settings: ~/.config/fldesktop/term.ini [Terminal]
#pragma once
#include <string>
#include <vector>

namespace fld { class Ini; }

namespace term {

enum TabsMode { TABS_AUTO, TABS_ALWAYS, TABS_NEVER };

struct Settings {
  std::string scheme = "dark";
  int font_size = 0;          // 0: the desktop's monospace size
  int scrollback = 10000;
  double opacity = 1.0;       // background opacity, 0.2 .. 1
  bool show_menubar = false;
  TabsMode tabs = TABS_AUTO;  // auto: the tab bar shows with two or more tabs
  std::string default_profile; // new tabs and windows use it; "" the settings above
};

// A named way to start a terminal: [Profile NAME] in term.ini. Empty or
// zero fields take the general settings.
struct Profile {
  std::string name;
  std::string command; // "" the user's shell
  std::string cwd;     // "" the current tab's folder
  std::string scheme;
  int font_size = 0;
  double opacity = 0; // 0: the general opacity
};

std::vector<Profile> load_profiles(const fld::Ini &ini);
// replaces all [Profile ...] groups
void store_profiles(const std::vector<Profile> &p, fld::Ini &ini);
const Profile *find_profile(const std::vector<Profile> &p, const std::string &name);

const char *tabs_mode_name(TabsMode m);
TabsMode parse_tabs_mode(const std::string &s);
double clamp_opacity(double o);
// Whether the tab bar is visible with `ntabs` tabs open.
bool tabs_visible(TabsMode m, int ntabs);

void load(const fld::Ini &ini, Settings &s);
void store(const Settings &s, fld::Ini &ini);

std::string config_path();
Settings load_file();
bool save_file(const Settings &s); // keeps other keys and comments in the file

} // namespace term
