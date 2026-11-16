// freedesktop.org Desktop Entry files
#pragma once
#include <string>
#include <vector>

namespace fld {

struct DesktopEntry {
  std::string id;        // e.g. "org.example.App.desktop"
  std::string path;      // full file path
  std::string type;      // Application, Link, Directory
  std::string name;      // localized
  std::string generic_name;
  std::string comment;
  std::string icon;
  std::string exec;
  std::string try_exec;
  std::string path_dir;  // Path=
  std::string url;       // for Type=Link
  std::vector<std::string> categories;
  std::vector<std::string> mime_types;
  std::vector<std::string> keywords;
  std::vector<std::string> only_show_in, not_show_in;
  bool terminal = false;
  bool no_display = false;
  bool hidden = false;

  bool load(const std::string &file, const std::string &id = "");
  // Should it be shown in menus for this desktop?
  bool visible() const;
  // Expand Exec into argv, substituting %f/%F/%u/%U etc.
  std::vector<std::string> command(const std::vector<std::string> &files = {}) const;
  // Launch it (wraps in a terminal if Terminal=true).
  bool launch(const std::vector<std::string> &files = {}) const;
  // Primary menu category (Development, Graphics, ... or "Other")
  std::string main_category() const;
};

// All application entries from $XDG_DATA_DIRS/applications (deduplicated by id,
// earlier dirs win), sorted by name.
std::vector<DesktopEntry> all_applications(bool include_hidden = false);
// Finds an application entry by desktop id (with or without ".desktop").
bool find_application(const std::string &id, DesktopEntry &out);
// Unescapes a desktop entry string value (\s \n \t \r \\).
std::string desktop_unescape(const std::string &s);
// Returns a value honouring the current locale (Key[ll_CC], Key[ll], Key).
std::string localized_key_lang();

} // namespace fld
