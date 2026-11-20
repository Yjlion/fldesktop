// fl-docs: reading members of zip (and rar, 7z, tar) files with libarchive
#pragma once
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace docs {

// "./a/../b//c" -> "b/c"; "" for a name that leaves the archive
std::string clean_member(const std::string &name);
// `href` (as written in a member at `base`, URL-encoded) to a member name
std::string resolve_member(const std::string &base, const std::string &href);

// The names of the regular files in the archive, in archive order.
bool list_members(const std::string &path, std::vector<std::string> &names, std::string &err);

// Reads the members `want` accepts into `out` (name -> bytes). Stops with an
// error past `max_total` bytes, or a member past `max_member`. With `prefix`,
// only the first `prefix` bytes of each member are read.
bool read_members(const std::string &path, const std::function<bool(const std::string &)> &want,
                  std::map<std::string, std::string> &out, std::string &err, size_t max_total = 512u << 20,
                  size_t max_member = 128u << 20, size_t prefix = 0);

// One member.
bool read_member(const std::string &path, const std::string &name, std::string &out, std::string &err);

} // namespace docs
