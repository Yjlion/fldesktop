// fl-settings: the XDG autostart entries (~/.config/autostart over
// $XDG_CONFIG_DIRS/autostart), as fl-autostart starts them. No FLTK here
// (unit tested).
#pragma once
#include <string>
#include <vector>

namespace autostart {

struct Entry {
  std::string id;          // file name, "foo.desktop"
  std::string name, comment, exec, icon;
  std::string user_path;   // the file in the user's autostart folder ("" if none)
  std::string system_path; // the system one it overrides ("" if none)
  bool enabled = true;     // not Hidden=true
};

// The entries for this desktop (OnlyShowIn/NotShowIn), sorted by name.
// dirs: the config directories, the user's first (fld::config_dirs()).
std::vector<Entry> list(const std::vector<std::string> &dirs);
// Turns an entry on or off: Hidden= in the user's copy, which is made from
// the system file when needed. A user copy that only hid a system entry is
// removed when it is turned on again.
bool set_enabled(const Entry &e, bool on, const std::string &user_dir, std::string &err);
// A new entry in the user's folder; the id comes from the name. Returns the
// new entry's id ("" on failure).
std::string add(const std::string &user_dir, const std::string &name, const std::string &exec,
                const std::string &icon, const std::string &comment, std::string &err);
// Removes the user's file; a system entry stays (turned off instead).
bool remove(const Entry &e, const std::string &user_dir, std::string &err);
// "My Tool 2" -> "my-tool-2"
std::string id_for(const std::string &name);

} // namespace autostart
