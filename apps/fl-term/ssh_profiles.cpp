#include "ssh_profiles.h"

#include "fld/ini.h"
#include "fld/util.h"

#include <algorithm>
#include <cstdlib>
#include <glob.h>
#include <cstring>
#include <set>

namespace ssh {

std::vector<std::string> build_argv(const Profile &p) {
  std::vector<std::string> a{"ssh"};
  if (p.port > 0) { a.push_back("-p"); a.push_back(std::to_string(p.port)); }
  if (!p.identity.empty()) { a.push_back("-i"); a.push_back(fld::expand_tilde(p.identity)); }
  if (!p.jump.empty()) { a.push_back("-J"); a.push_back(p.jump); }
  for (auto &f : p.forwards) {
    std::string t = fld::trim(f);
    if (t.size() < 2) continue;
    char kind = (char)toupper((unsigned char)t[0]);
    if (kind != 'L' && kind != 'R' && kind != 'D') continue;
    a.push_back(std::string("-") + kind);
    a.push_back(fld::trim(t.substr(1)));
  }
  for (auto &o : fld::shell_split(p.options)) a.push_back(o);
  if (!p.command.empty()) a.push_back("-t"); // the command gets a terminal too
  a.push_back(p.target());
  if (!p.command.empty()) a.push_back(p.command);
  return a;
}

Profile parse_target(const std::string &s_in) {
  Profile p;
  std::string s = fld::trim(s_in);
  if (fld::starts_with(s, "ssh://")) s = s.substr(6);
  while (!s.empty() && s.back() == '/') s.pop_back();
  size_t at = s.rfind('@');
  if (at != std::string::npos) {
    p.user = s.substr(0, at);
    s = s.substr(at + 1);
  }
  // host:port, but not a bare IPv6 address; [v6]:port
  if (!s.empty() && s[0] == '[') {
    size_t close = s.find(']');
    if (close != std::string::npos) {
      if (close + 1 < s.size() && s[close + 1] == ':') p.port = atoi(s.c_str() + close + 2);
      s = s.substr(1, close - 1);
    }
  } else if (std::count(s.begin(), s.end(), ':') == 1) {
    size_t c = s.find(':');
    p.port = atoi(s.c_str() + c + 1);
    s = s.substr(0, c);
  }
  p.host = s;
  p.name = p.target();
  return p;
}

static void read_config(const std::string &path, const std::string &ssh_dir, std::vector<Profile> &out,
                        std::set<std::string> &seen, int depth) {
  if (depth > 8) return;
  std::string text;
  if (!fld::read_file(path, text)) return;
  std::vector<size_t> current; // indices into out for the active Host block
  for (auto &raw : fld::split(text, '\n', true)) {
    std::string line = fld::trim(raw);
    if (line.empty() || line[0] == '#') continue;
    // "Key value" or "Key=value"
    size_t sep = line.find_first_of(" \t=");
    if (sep == std::string::npos) continue;
    std::string key = fld::lower(line.substr(0, sep));
    std::string val = fld::trim(line.substr(sep + 1));
    if (!val.empty() && val[0] == '=') val = fld::trim(val.substr(1));
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
    if (key == "include") {
      for (auto &pat : fld::shell_split(val)) {
        std::string p = fld::expand_tilde(pat);
        if (p.empty() || p[0] != '/') p = ssh_dir + "/" + p;
        glob_t g{};
        if (glob(p.c_str(), 0, nullptr, &g) == 0)
          for (size_t i = 0; i < g.gl_pathc; i++) read_config(g.gl_pathv[i], ssh_dir, out, seen, depth + 1);
        globfree(&g);
      }
      continue;
    }
    if (key == "host") {
      current.clear();
      for (auto &h : fld::shell_split(val)) {
        if (h.find_first_of("*?!") != std::string::npos) continue;
        if (seen.count(h)) continue;
        seen.insert(h);
        Profile p;
        p.name = h;
        p.host = h; // ssh resolves HostName, User, Port... itself
        p.from_ssh_config = true;
        out.push_back(p);
        current.push_back(out.size() - 1);
      }
      continue;
    }
    if (key == "match") { current.clear(); continue; }
    for (size_t i : current) {
      // shown in the connection list; ssh applies them anyway
      if (key == "user") out[i].user = val;
      else if (key == "port") out[i].port = atoi(val.c_str());
    }
  }
}

std::vector<Profile> read_ssh_config(const std::string &path, const std::string &ssh_dir) {
  std::vector<Profile> out;
  std::set<std::string> seen;
  read_config(path, ssh_dir, out, seen, 0);
  // the user and port come from ssh_config; keep the alias as the target
  for (auto &p : out) {
    p.user.clear();
    p.port = 0;
  }
  return out;
}

static const char *kPrefix = "Profile ";

std::vector<Profile> load_profiles(const std::string &ini_path) {
  fld::Ini ini;
  ini.load(ini_path);
  std::vector<Profile> v;
  for (auto &g : ini.groups()) {
    if (!fld::starts_with(g, kPrefix)) continue;
    Profile p;
    p.name = g.substr(strlen(kPrefix));
    p.host = ini.get(g, "Host");
    if (p.host.empty()) continue;
    p.user = ini.get(g, "User");
    p.port = ini.get_int(g, "Port", 0);
    p.identity = ini.get(g, "IdentityFile");
    p.jump = ini.get(g, "ProxyJump");
    p.forwards = fld::split(ini.get(g, "Forwards"), ';');
    p.options = ini.get(g, "Options");
    p.command = ini.get(g, "Command");
    p.scheme = ini.get(g, "ColorScheme");
    v.push_back(p);
  }
  return v;
}

bool save_profiles(const std::string &ini_path, const std::vector<Profile> &profiles) {
  fld::Ini ini;
  ini.load(ini_path);
  for (auto &g : ini.groups())
    if (fld::starts_with(g, kPrefix)) ini.remove_group(g);
  for (auto &p : profiles) {
    if (p.from_ssh_config || p.name.empty()) continue;
    std::string g = kPrefix + p.name;
    ini.set(g, "Host", p.host);
    if (!p.user.empty()) ini.set(g, "User", p.user);
    if (p.port > 0) ini.set_int(g, "Port", p.port);
    if (!p.identity.empty()) ini.set(g, "IdentityFile", p.identity);
    if (!p.jump.empty()) ini.set(g, "ProxyJump", p.jump);
    if (!p.forwards.empty()) ini.set(g, "Forwards", fld::join(p.forwards, ";"));
    if (!p.options.empty()) ini.set(g, "Options", p.options);
    if (!p.command.empty()) ini.set(g, "Command", p.command);
    if (!p.scheme.empty()) ini.set(g, "ColorScheme", p.scheme);
  }
  return ini.save(ini_path);
}

std::vector<Profile> all_profiles(const std::string &ini_path, const std::string &ssh_config,
                                  const std::string &ssh_dir) {
  std::vector<Profile> v = load_profiles(ini_path);
  std::set<std::string> names;
  for (auto &p : v) names.insert(p.name);
  for (auto &p : read_ssh_config(ssh_config, ssh_dir))
    if (!names.count(p.name)) v.push_back(p);
  return v;
}

const Profile *find(const std::vector<Profile> &v, const std::string &name) {
  for (auto &p : v)
    if (p.name == name) return &p;
  return nullptr;
}

} // namespace ssh
