// Flatpak permissions: the app's own [Context] (from its metadata) and the
// user's overrides (~/.local/share/flatpak/overrides/<app id>, the file
// `flatpak override --user` writes).
#pragma once
#include <map>
#include <string>
#include <vector>

namespace fp {

struct PermItem {
  std::string category; // shared, sockets, devices, features
  std::string key;      // network, x11, dri, bluetooth...
  std::string label;    // "Network access"
};

// the switches the permissions dialog offers
const std::vector<PermItem> &perm_items();

struct Permissions {
  // category -> key -> granted by the app's metadata
  std::map<std::string, std::map<std::string, bool>> app;
  // category -> key -> overridden value (absent = not overridden)
  std::map<std::string, std::map<std::string, bool>> over;
  std::vector<std::string> app_filesystems;  // "home", "xdg-download:ro", ...
  std::map<std::string, bool> fs_over;       // filesystem -> granted (false = "!fs")
  std::map<std::string, std::string> app_env, env_over;

  bool effective(const std::string &cat, const std::string &key) const;
  bool overridden(const std::string &cat, const std::string &key) const;
  // set the value; equal to the app's value removes the override
  void set(const std::string &cat, const std::string &key, bool on);
  // the filesystems in effect: app's ones minus "!x" overrides plus added
  std::vector<std::string> filesystems() const;
  void add_filesystem(const std::string &fs);    // grant (override)
  void remove_filesystem(const std::string &fs); // revoke (or drop an added one)
  void reset();                                  // drop all overrides
};

// parses the app's metadata keyfile text ([Context], [Environment])
void parse_metadata(const std::string &text, Permissions &p);
// parses an overrides keyfile text
void parse_overrides(const std::string &text, Permissions &p);
// writes the overrides into an existing overrides text (keeps other groups/keys)
std::string write_overrides(const std::string &existing, const Permissions &p);
// a filesystem entry for display: "xdg-download:ro" -> "Downloads folder (read only)"
std::string filesystem_label(const std::string &fs);

} // namespace fp
