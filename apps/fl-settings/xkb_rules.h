// Parser for xkeyboard-config's base.lst (layouts, variants, options)
#pragma once
#include <map>
#include <string>
#include <vector>

namespace xkb {

struct Item {
  std::string name, description;
};

struct Rules {
  std::vector<Item> models, layouts;
  std::map<std::string, std::vector<Item>> variants; // by layout
  std::vector<Item> options;                          // e.g. grp:alt_shift_toggle
  bool parse(const std::string &text);
  bool load(const std::string &path = "/usr/share/X11/xkb/rules/base.lst");
  std::string layout_description(const std::string &layout, const std::string &variant = "") const;
  // options of a group, e.g. "grp" -> the layout switching keys
  std::vector<Item> options_of(const std::string &group) const;
};

// "us,de" + ",nodeadkeys" -> {{us,""},{de,nodeadkeys}}
std::vector<std::pair<std::string, std::string>> split_layouts(const std::string &layouts, const std::string &variants);
void join_layouts(const std::vector<std::pair<std::string, std::string>> &v, std::string &layouts, std::string &variants);

} // namespace xkb
