#include "services_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <sstream>

#include "fld/util.h"

namespace svc {

Init detect_init() {
  if (fld::is_dir("/run/systemd/system")) return Init::Systemd;
  if (fld::is_dir("/run/openrc") || !fld::which("rc-service").empty()) return Init::OpenRC;
  return Init::Unknown;
}

static std::string strip_suffix(const std::string &unit) {
  return fld::ends_with(unit, ".service") ? unit.substr(0, unit.size() - 8) : unit;
}

std::vector<Service> parse_systemctl_units(const std::string &out) {
  std::vector<Service> v;
  for (const std::string &raw : fld::split(out, '\n')) {
    std::string line = fld::trim(raw);
    // a failed unit may start with a bullet
    if (fld::starts_with(line, "\xe2\x97\x8f")) line = fld::trim(line.substr(3));
    if (line.empty()) continue;
    std::istringstream in(line);
    std::string unit, load, active, sub;
    if (!(in >> unit >> load >> active >> sub)) continue;
    if (!fld::ends_with(unit, ".service")) continue;
    Service s;
    s.name = strip_suffix(unit);
    s.active = active;
    s.sub = sub;
    std::string desc;
    std::getline(in, desc);
    s.description = fld::trim(desc);
    if (load == "not-found") continue; // listed only because something refers to it
    v.push_back(s);
  }
  return v;
}

void merge_unit_files(std::vector<Service> &list, const std::string &out) {
  std::map<std::string, size_t> idx;
  for (size_t i = 0; i < list.size(); i++) idx[list[i].name] = i;
  for (const std::string &raw : fld::split(out, '\n')) {
    std::istringstream in(fld::trim(raw));
    std::string unit, state;
    if (!(in >> unit >> state)) continue;
    if (!fld::ends_with(unit, ".service")) continue;
    std::string name = strip_suffix(unit);
    // templates (getty@.service) cannot be started as they are
    if (fld::ends_with(name, "@")) continue;
    auto it = idx.find(name);
    if (it != idx.end()) {
      list[it->second].enabled = state;
    } else {
      Service s;
      s.name = name;
      s.active = "inactive";
      s.sub = "dead";
      s.enabled = state;
      idx[name] = list.size();
      list.push_back(s);
    }
  }
  std::sort(list.begin(), list.end(), [](const Service &a, const Service &b) { return a.name < b.name; });
}

std::vector<Service> parse_rc_status(const std::string &out) {
  std::vector<Service> v;
  std::map<std::string, bool> seen;
  for (const std::string &raw : fld::split(out, '\n')) {
    std::string line = fld::trim(raw);
    if (line.empty() || fld::starts_with(line, "Runlevel:") || fld::starts_with(line, "Dynamic Runlevel:")) continue;
    size_t open = line.rfind('[');
    size_t close = line.rfind(']');
    if (open == std::string::npos || close == std::string::npos || close < open) continue;
    std::string name = fld::trim(line.substr(0, open));
    std::string state = fld::trim(line.substr(open + 1, close - open - 1));
    // "started  00:12:34 (0)" for supervised services
    size_t sp = state.find(' ');
    if (sp != std::string::npos) state = state.substr(0, sp);
    if (name.empty() || seen[name]) continue;
    seen[name] = true;
    Service s;
    s.name = name;
    s.active = state;
    v.push_back(s);
  }
  std::sort(v.begin(), v.end(), [](const Service &a, const Service &b) { return a.name < b.name; });
  return v;
}

void merge_rc_update(std::vector<Service> &list, const std::string &out) {
  std::map<std::string, size_t> idx;
  for (size_t i = 0; i < list.size(); i++) idx[list[i].name] = i;
  for (const std::string &raw : fld::split(out, '\n')) {
    size_t bar = raw.find('|');
    if (bar == std::string::npos) continue;
    std::string name = fld::trim(raw.substr(0, bar));
    std::string levels = fld::trim(raw.substr(bar + 1));
    if (name.empty()) continue;
    auto it = idx.find(name);
    if (it != idx.end()) {
      list[it->second].enabled = levels;
    } else {
      Service s;
      s.name = name;
      s.active = "stopped";
      s.enabled = levels;
      idx[name] = list.size();
      list.push_back(s);
    }
  }
  std::sort(list.begin(), list.end(), [](const Service &a, const Service &b) { return a.name < b.name; });
}

bool valid_name(const std::string &n) {
  if (n.empty() || n.size() > 200 || n[0] == '-' || n[0] == '.') return false;
  for (char c : n)
    if (!isalnum((unsigned char)c) && !strchr("@._-:\\", c)) return false;
  return true;
}

std::string state_label(const Service &s) {
  if (s.failed()) return "Failed";
  if (s.active == "activating") return "Starting";
  if (s.active == "deactivating") return "Stopping";
  if (s.running()) return "Running";
  if (s.sub == "exited" && s.active == "active") return "Done";
  if (s.active == "active") return "Active";
  return "Stopped";
}

} // namespace svc
