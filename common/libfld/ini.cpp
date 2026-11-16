#include "fld/ini.h"
#include "fld/util.h"

#include <cstdlib>

namespace fld {

bool Ini::load(const std::string &path) {
  std::string text;
  if (!read_file(path, text)) return false;
  return parse(text);
}

bool Ini::parse(const std::string &text) {
  groups_.clear();
  Group *cur = nullptr;
  for (auto &raw : split(text, '\n')) {
    std::string line = trim(raw);
    if (line.empty() || line[0] == '#' || line[0] == ';') continue;
    if (line.front() == '[' && line.back() == ']') {
      std::string name = line.substr(1, line.size() - 2);
      cur = find(name);
      if (!cur) {
        groups_.push_back({name, {}});
        cur = &groups_.back();
      }
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    if (!cur) {
      groups_.push_back({"", {}});
      cur = &groups_.back();
    }
    std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
    bool replaced = false;
    for (auto &e : cur->entries)
      if (e.first == k) { e.second = v; replaced = true; }
    if (!replaced) cur->entries.push_back({k, v});
  }
  return true;
}

std::string Ini::to_string() const {
  std::string out;
  for (auto &g : groups_) {
    if (!out.empty()) out += "\n";
    if (!g.name.empty()) out += "[" + g.name + "]\n";
    for (auto &e : g.entries) out += e.first + "=" + e.second + "\n";
  }
  return out;
}

bool Ini::save(const std::string &path) const {
  mkdir_p(dirname(path));
  return write_file(path, to_string());
}

Ini::Group *Ini::find(const std::string &g) {
  for (auto &x : groups_) if (x.name == g) return &x;
  return nullptr;
}

const Ini::Group *Ini::find(const std::string &g) const {
  for (auto &x : groups_) if (x.name == g) return &x;
  return nullptr;
}

bool Ini::has(const std::string &group, const std::string &key) const {
  auto g = find(group);
  if (!g) return false;
  for (auto &e : g->entries) if (e.first == key) return true;
  return false;
}

std::string Ini::get(const std::string &group, const std::string &key,
                     const std::string &def) const {
  auto g = find(group);
  if (!g) return def;
  for (auto &e : g->entries) if (e.first == key) return e.second;
  return def;
}

int Ini::get_int(const std::string &group, const std::string &key, int def) const {
  std::string v = get(group, key);
  if (v.empty()) return def;
  char *end;
  long r = strtol(v.c_str(), &end, 10);
  return *end ? def : (int)r;
}

double Ini::get_double(const std::string &group, const std::string &key, double def) const {
  std::string v = get(group, key);
  if (v.empty()) return def;
  char *end;
  double r = strtod(v.c_str(), &end);
  return *end ? def : r;
}

bool Ini::get_bool(const std::string &group, const std::string &key, bool def) const {
  std::string v = lower(get(group, key));
  if (v == "true" || v == "1" || v == "yes" || v == "on") return true;
  if (v == "false" || v == "0" || v == "no" || v == "off") return false;
  return def;
}

void Ini::set(const std::string &group, const std::string &key, const std::string &value) {
  Group *g = find(group);
  if (!g) {
    groups_.push_back({group, {}});
    g = &groups_.back();
  }
  for (auto &e : g->entries)
    if (e.first == key) { e.second = value; return; }
  g->entries.push_back({key, value});
}

void Ini::set_int(const std::string &group, const std::string &key, int v) {
  set(group, key, std::to_string(v));
}

void Ini::set_bool(const std::string &group, const std::string &key, bool v) {
  set(group, key, v ? "true" : "false");
}

void Ini::remove(const std::string &group, const std::string &key) {
  Group *g = find(group);
  if (!g) return;
  for (auto it = g->entries.begin(); it != g->entries.end(); ++it)
    if (it->first == key) { g->entries.erase(it); return; }
}

std::vector<std::string> Ini::groups() const {
  std::vector<std::string> v;
  for (auto &g : groups_) v.push_back(g.name);
  return v;
}

std::vector<std::string> Ini::keys(const std::string &group) const {
  std::vector<std::string> v;
  if (auto g = find(group)) for (auto &e : g->entries) v.push_back(e.first);
  return v;
}

} // namespace fld
