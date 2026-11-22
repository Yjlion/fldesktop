#include "users_model.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>

#include "fld/util.h"

namespace fld {
namespace users {

bool Group::has(const std::string &user) const {
  return std::find(members.begin(), members.end(), user) != members.end();
}

bool Account::in(const std::string &group) const {
  return std::find(groups.begin(), groups.end(), group) != groups.end();
}

std::vector<Group> parse_group(const std::string &text) {
  std::vector<Group> out;
  for (auto &line : split(text, '\n')) {
    if (line.empty() || line[0] == '#') continue;
    auto f = split(line, ':', true);
    if (f.size() < 3 || f[0].empty()) continue;
    Group g;
    g.name = f[0];
    g.gid = (gid_t)strtoul(f[2].c_str(), nullptr, 10);
    if (f.size() > 3)
      for (auto &m : split(f[3], ','))
        if (!trim(m).empty()) g.members.push_back(trim(m));
    out.push_back(g);
  }
  return out;
}

std::vector<Account> parse_passwd(const std::string &text) {
  std::vector<Account> out;
  for (auto &line : split(text, '\n')) {
    if (line.empty() || line[0] == '#' || line[0] == '+' || line[0] == '-') continue;
    auto f = split(line, ':', true);
    if (f.size() < 7 || f[0].empty()) continue;
    Account a;
    a.name = f[0];
    a.uid = (uid_t)strtoul(f[2].c_str(), nullptr, 10);
    a.gid = (gid_t)strtoul(f[3].c_str(), nullptr, 10);
    a.real_name = f[4].substr(0, f[4].find(','));
    a.home = f[5];
    a.shell = f[6];
    if (a.uid < kMinUid || a.uid >= kMaxUid) continue;
    if (ends_with(a.shell, "nologin") || ends_with(a.shell, "false")) continue;
    out.push_back(a);
  }
  std::sort(out.begin(), out.end(), [](const Account &a, const Account &b) { return a.name < b.name; });
  return out;
}

void attach_groups(std::vector<Account> &accounts, const std::vector<Group> &groups, const std::string &admin) {
  for (auto &a : accounts) {
    a.groups.clear();
    for (auto &g : groups)
      if (g.has(a.name)) a.groups.push_back(g.name);
    a.admin = !admin.empty() && a.in(admin);
  }
}

std::string admin_group(const std::vector<Group> &groups) {
  for (const char *n : {"wheel", "sudo"})
    if (find_group(groups, n)) return n;
  return "";
}

const Group *find_group(const std::vector<Group> &groups, const std::string &name) {
  for (auto &g : groups)
    if (g.name == name) return &g;
  return nullptr;
}

const Account *find_account(const std::vector<Account> &accounts, const std::string &name) {
  for (auto &a : accounts)
    if (a.name == name) return &a;
  return nullptr;
}

std::vector<std::string> editable_groups(const std::vector<Group> &groups, const std::vector<Account> &accounts,
                                         const std::string &admin) {
  static const char *usual[] = {"adm",     "audio",   "cdrom",  "dialout", "docker", "games",  "input",
                                "kvm",     "libvirt", "lp",     "lpadmin", "network", "optical", "plugdev",
                                "power",   "render",  "scanner", "storage", "users",  "uucp",   "video",
                                "vboxusers", "wireshark"};
  std::set<std::string> own;  // user private groups
  for (auto &a : accounts) own.insert(a.name);
  std::set<std::string> out;
  for (auto &g : groups) {
    if (g.name == admin || own.count(g.name)) continue;
    bool listed = std::find(std::begin(usual), std::end(usual), g.name) != std::end(usual);
    if (listed || (g.gid >= kMinUid && g.gid < kMaxUid)) out.insert(g.name);
  }
  return std::vector<std::string>(out.begin(), out.end());
}

int admin_count(const std::vector<Account> &accounts) {
  int n = 0;
  for (auto &a : accounts) n += a.admin;
  return n;
}

bool valid_user_name(const std::string &name, std::string &err) {
  if (name.empty()) {
    err = "Enter a user name.";
    return false;
  }
  if (name.size() > 32) {
    err = "The user name is longer than 32 characters.";
    return false;
  }
  unsigned char c0 = (unsigned char)name[0];
  if (!(islower(c0) || c0 == '_')) {
    err = "The user name has to start with a lower case letter (a–z).";
    return false;
  }
  for (unsigned char c : name)
    if (!(islower(c) || isdigit(c) || c == '_' || c == '-') || c >= 0x80) {
      err = "The user name can only have lower case letters (a–z), digits, - and _.";
      return false;
    }
  for (const char *r : {"root", "nobody", "daemon", "bin", "sys"})
    if (name == r) {
      err = "\"" + name + "\" is reserved for the system.";
      return false;
    }
  return true;
}

bool valid_full_name(const std::string &name, std::string &err) {
  if (name.size() > 128) {
    err = "The full name is too long.";
    return false;
  }
  for (unsigned char c : name)
    if (c == ':' || c == ',' || c == '=' || c == '\\' || c < 0x20 || c == 0x7f) {
      err = "The full name cannot have : , = \\ or control characters.";
      return false;
    }
  return true;
}

bool valid_password(const std::string &pw, std::string &err) {
  if (pw.empty()) {
    err = "Enter a password.";
    return false;
  }
  if (pw.size() > 512) {
    err = "The password is too long.";
    return false;
  }
  for (unsigned char c : pw)
    if (c == '\n' || c == '\r' || c == 0) {
      err = "The password cannot have line breaks.";
      return false;
    }
  return true;
}

std::string suggest_user_name(const std::string &full_name) {
  std::string first = trim(full_name);
  size_t sp = first.find_first_of(" \t");
  if (sp != std::string::npos) first = first.substr(0, sp);
  std::string out;
  for (unsigned char c : first) {
    if (isalpha(c) && c < 0x80) out += (char)tolower(c);
    else if (isdigit(c) && !out.empty()) out += (char)c;
    if (out.size() >= 32) break;
  }
  return out;
}

int password_strength(const std::string &pw) {
  bool lo = false, up = false, dig = false, other = false;
  for (unsigned char c : pw) {
    if (islower(c)) lo = true;
    else if (isupper(c)) up = true;
    else if (isdigit(c)) dig = true;
    else other = true;
  }
  int kinds = lo + up + dig + other;
  size_t n = pw.size();
  if (n < 8) return 0;
  if (n >= 16 && kinds >= 2) return 3;
  if (n >= 12 && kinds >= 3) return 3;
  if (kinds >= 3 || (n >= 12 && kinds >= 2)) return 2;
  return 1;
}

const char *strength_text(int s) {
  static const char *t[] = {"Weak: use 8 or more characters", "Fair", "Good", "Strong"};
  return t[std::max(0, std::min(3, s))];
}

std::string picture_path(const std::string &user) { return "/var/lib/AccountsService/icons/" + user; }

namespace {
// [section] header name of a TOML line, "" when it is not one
std::string section_of(const std::string &line) {
  std::string t = trim(line);
  if (t.size() < 3 || t[0] != '[' || t[1] == '[') return "";
  size_t e = t.find(']');
  if (e == std::string::npos) return "";
  return trim(t.substr(1, e - 1));
}

std::string toml_string(const std::string &v) {
  std::string out = "\"";
  for (char c : v) {
    if (c == '"' || c == '\\') out += '\\';
    out += c;
  }
  return out + "\"";
}
}  // namespace

std::string autologin_user(const std::string &toml) {
  bool in = false;
  for (auto &line : split(toml, '\n', true)) {
    std::string s = section_of(line);
    if (!s.empty()) {
      in = s == "initial_session";
      continue;
    }
    if (!in) continue;
    std::string t = trim(line);
    if (!starts_with(t, "user")) continue;
    size_t eq = t.find('=');
    if (eq == std::string::npos || trim(t.substr(0, eq)) != "user") continue;
    std::string v = trim(t.substr(eq + 1));
    size_t hash = v.find('#');
    if (!v.empty() && v[0] != '"' && hash != std::string::npos) v = trim(v.substr(0, hash));
    if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'')) {
      size_t end = v.find(v[0], 1);
      v = v.substr(1, end == std::string::npos ? std::string::npos : end - 1);
    }
    return v;
  }
  return "";
}

std::string set_autologin(const std::string &toml, const std::string &user, const std::string &command) {
  const std::string marker = "# set by fldesktop Settings > Users: log this user in automatically at boot";
  std::vector<std::string> keep;
  bool in = false;
  for (auto &line : split(toml, '\n', true)) {
    std::string s = section_of(line);
    if (!s.empty()) {
      in = s == "initial_session";
      // our comment goes with the section
      if (in && !keep.empty() && trim(keep.back()) == marker) keep.pop_back();
    }
    if (!in) keep.push_back(line);
  }
  // drop trailing empty lines, then add the section at the end
  while (!keep.empty() && trim(keep.back()).empty()) keep.pop_back();
  std::string out = join(keep, "\n");
  if (!out.empty()) out += "\n";
  if (!user.empty()) {
    if (!out.empty()) out += "\n";
    out += marker + "\n";
    out += "[initial_session]\n";
    out += "command = " + toml_string(command) + "\n";
    out += "user = " + toml_string(user) + "\n";
  }
  return out;
}

}  // namespace users
}  // namespace fld
