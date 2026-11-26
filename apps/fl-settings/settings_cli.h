// fl-settings on the command line (no FLTK; unit tested):
//   fl-settings --get wm.behaviour.workspaces
//   fl-settings --set wm.behaviour.workspaces=6
//   fl-settings --unset wm.behaviour.workspaces
//   fl-settings --export settings.tar.gz / --import settings.tar.gz
// The first part names a file in ~/.config/fldesktop (wm -> wm.ini), the
// last the key, the rest the section.
#pragma once
#include <string>

namespace settings_cli {

struct Key {
  std::string file, section, key; // file: the path of the .ini
};
// "wm.behaviour.workspaces" -> ~/.config/fldesktop/wm.ini, behaviour, workspaces
bool parse_key(const std::string &spec, const std::string &config_dir, Key &out, std::string &err);
// Runs one command (args after the program name). Returns the exit status;
// output goes to stdout, errors to stderr. Returns -1 when args are no CLI command.
int run(int argc, char **argv, const std::string &config_dir);
// The archive's members are all under fldesktop/ (no absolute paths, no "..")
bool safe_members(const std::string &tar_listing, std::string &err);

} // namespace settings_cli
