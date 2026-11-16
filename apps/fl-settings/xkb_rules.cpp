#include "xkb_rules.h"

#include <sstream>

#include "fld/util.h"

namespace xkb {

bool Rules::parse(const std::string &text) {
  std::istringstream in(text);
  std::string line, section;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    if (line[0] == '!') {
      section = fld::trim(line.substr(1));
      continue;
    }
    std::string l = fld::trim(line);
    size_t sp = l.find_first_of(" \t");
    if (sp == std::string::npos) continue;
    Item it;
    it.name = l.substr(0, sp);
    std::string rest = fld::trim(l.substr(sp));
    if (section == "variant") {
      // "nodeadkeys      de: German (no dead keys)"
      size_t colon = rest.find(':');
      if (colon == std::string::npos) continue;
      std::string layout = rest.substr(0, colon);
      it.description = fld::trim(rest.substr(colon + 1));
      variants[layout].push_back(it);
    } else {
      it.description = rest;
      if (section == "model") models.push_back(it);
      else if (section == "layout") layouts.push_back(it);
      else if (section == "option") options.push_back(it);
    }
  }
  return !layouts.empty();
}

bool Rules::load(const std::string &path) {
  std::string text;
  return fld::read_file(path, text) && parse(text);
}

std::string Rules::layout_description(const std::string &layout, const std::string &variant) const {
  if (!variant.empty()) {
    auto it = variants.find(layout);
    if (it != variants.end())
      for (auto &v : it->second)
        if (v.name == variant) return v.description;
  }
  for (auto &l : layouts)
    if (l.name == layout) return l.description;
  return variant.empty() ? layout : layout + " (" + variant + ")";
}

std::vector<Item> Rules::options_of(const std::string &group) const {
  std::vector<Item> v;
  for (auto &o : options)
    if (fld::starts_with(o.name, group + ":")) v.push_back(o);
  return v;
}

std::vector<std::pair<std::string, std::string>> split_layouts(const std::string &layouts, const std::string &variants) {
  auto l = fld::split(layouts, ',', true), v = fld::split(variants, ',', true);
  std::vector<std::pair<std::string, std::string>> out;
  for (size_t i = 0; i < l.size(); i++) {
    std::string name = fld::trim(l[i]);
    if (name.empty()) continue;
    out.push_back({name, i < v.size() ? fld::trim(v[i]) : ""});
  }
  return out;
}

void join_layouts(const std::vector<std::pair<std::string, std::string>> &v, std::string &layouts, std::string &variants) {
  layouts.clear();
  variants.clear();
  bool any_variant = false;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) { layouts += ","; variants += ","; }
    layouts += v[i].first;
    variants += v[i].second;
    if (!v[i].second.empty()) any_variant = true;
  }
  if (!any_variant) variants.clear();
}

} // namespace xkb
