#include "fld/desktop_entry.h"
#include "fld/ini.h"
#include "fld/util.h"

#include <algorithm>
#include <map>
#include <set>
#include <strings.h>

namespace fld {

std::string desktop_unescape(const std::string &s) {
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char c = s[++i];
      switch (c) {
        case 's': r += ' '; break;
        case 'n': r += '\n'; break;
        case 't': r += '\t'; break;
        case 'r': r += '\r'; break;
        case '\\': r += '\\'; break;
        default: r += '\\'; r += c; break;
      }
    } else r += s[i];
  }
  return r;
}

std::string localized_key_lang() {
  std::string l = getenv_or("LC_ALL", getenv_or("LC_MESSAGES", getenv_or("LANG", "")));
  size_t p = l.find_first_of(".@");
  if (p != std::string::npos) l = l.substr(0, p);
  if (l == "C" || l == "POSIX") return "";
  return l;
}

static std::string loc_get(const Ini &ini, const std::string &g, const std::string &k) {
  std::string lang = localized_key_lang();
  if (!lang.empty()) {
    if (ini.has(g, k + "[" + lang + "]")) return ini.get(g, k + "[" + lang + "]");
    size_t u = lang.find('_');
    if (u != std::string::npos) {
      std::string ll = lang.substr(0, u);
      if (ini.has(g, k + "[" + ll + "]")) return ini.get(g, k + "[" + ll + "]");
    }
  }
  return ini.get(g, k);
}

static std::vector<std::string> list_value(const std::string &v) {
  std::vector<std::string> r;
  for (auto &x : split(v, ';')) {
    std::string t = trim(x);
    if (!t.empty()) r.push_back(desktop_unescape(t));
  }
  return r;
}

bool DesktopEntry::load(const std::string &file, const std::string &id_) {
  Ini ini;
  if (!ini.load(file)) return false;
  const std::string g = "Desktop Entry";
  if (!ini.has(g, "Type") && !ini.has(g, "Name")) return false;
  path = file;
  id = id_.empty() ? basename(file) : id_;
  type = ini.get(g, "Type", "Application");
  name = desktop_unescape(loc_get(ini, g, "Name"));
  generic_name = desktop_unescape(loc_get(ini, g, "GenericName"));
  comment = desktop_unescape(loc_get(ini, g, "Comment"));
  icon = desktop_unescape(ini.get(g, "Icon"));
  exec = ini.get(g, "Exec");
  try_exec = desktop_unescape(ini.get(g, "TryExec"));
  path_dir = desktop_unescape(ini.get(g, "Path"));
  url = desktop_unescape(ini.get(g, "URL"));
  categories = list_value(ini.get(g, "Categories"));
  mime_types = list_value(ini.get(g, "MimeType"));
  keywords = list_value(loc_get(ini, g, "Keywords"));
  only_show_in = list_value(ini.get(g, "OnlyShowIn"));
  not_show_in = list_value(ini.get(g, "NotShowIn"));
  terminal = ini.get_bool(g, "Terminal");
  no_display = ini.get_bool(g, "NoDisplay");
  hidden = ini.get_bool(g, "Hidden");
  if (name.empty()) name = id;
  actions.clear();
  for (auto &a : list_value(ini.get(g, "Actions"))) {
    std::string ag = "Desktop Action " + a;
    if (!ini.has(ag, "Name")) continue;
    DesktopAction da;
    da.id = a;
    da.name = desktop_unescape(loc_get(ini, ag, "Name"));
    da.icon = desktop_unescape(ini.get(ag, "Icon"));
    da.exec = ini.get(ag, "Exec");
    actions.push_back(da);
  }
  return true;
}

static std::vector<std::string> current_desktops() {
  return split(getenv_or("XDG_CURRENT_DESKTOP", "fldesktop"), ':');
}

bool DesktopEntry::visible() const {
  if (hidden || no_display) return false;
  auto desks = current_desktops();
  if (!only_show_in.empty()) {
    bool ok = false;
    for (auto &d : only_show_in)
      if (std::find(desks.begin(), desks.end(), d) != desks.end()) ok = true;
    if (!ok) return false;
  }
  for (auto &d : not_show_in)
    if (std::find(desks.begin(), desks.end(), d) != desks.end()) return false;
  if (!try_exec.empty() && which(try_exec).empty()) return false;
  return true;
}

std::vector<std::string> DesktopEntry::command(const std::vector<std::string> &files) const {
  // The Exec value is first unescaped as a string, then split with quoting rules.
  std::vector<std::string> words = shell_split(desktop_unescape(exec));
  std::vector<std::string> out;
  bool used_files = false;
  for (auto &w : words) {
    if (w == "%f" || w == "%u") {
      if (!files.empty()) out.push_back(files[0]);
      used_files = true;
      continue;
    }
    if (w == "%F" || w == "%U") {
      for (auto &f : files) out.push_back(f);
      used_files = true;
      continue;
    }
    if (w == "%i") {
      if (!icon.empty()) { out.push_back("--icon"); out.push_back(icon); }
      continue;
    }
    std::string r;
    for (size_t i = 0; i < w.size(); i++) {
      if (w[i] == '%' && i + 1 < w.size()) {
        char c = w[++i];
        if (c == '%') r += '%';
        else if (c == 'c') r += name;
        else if (c == 'k') r += path;
        else if ((c == 'f' || c == 'u') && !files.empty()) { r += files[0]; used_files = true; }
        // deprecated / unknown codes are dropped
      } else r += w[i];
    }
    out.push_back(r);
  }
  (void)used_files;
  return out;
}

bool DesktopEntry::launch(const std::vector<std::string> &files) const {
  if (type == "Link" && !url.empty()) return spawn({"xdg-open", url});
  std::vector<std::string> argv = command(files);
  if (argv.empty()) return false;
  if (terminal) {
    std::vector<std::string> t = shell_split(terminal_command());
    t.push_back("-e");
    t.insert(t.end(), argv.begin(), argv.end());
    argv = t;
  }
  return spawn(argv, path_dir.empty() ? "" : expand_tilde(path_dir));
}

bool DesktopEntry::launch_action(const std::string &action_id) const {
  for (auto &a : actions) {
    if (a.id != action_id) continue;
    if (a.exec.empty()) return false; // D-Bus activated actions are not supported
    DesktopEntry e = *this;
    e.exec = a.exec;
    return e.launch();
  }
  return false;
}

std::string DesktopEntry::main_category() const {
  static const char *mains[] = {"AudioVideo", "Development", "Education", "Game",
                                "Graphics", "Network", "Office", "Science",
                                "Settings", "System", "Utility"};
  for (auto &c : categories) {
    if (c == "Audio" || c == "Video") return "AudioVideo";
    for (auto m : mains) if (c == m) return m;
  }
  return "Other";
}

static void scan_apps(const std::string &base, const std::string &dir, const std::string &prefix,
                      std::map<std::string, DesktopEntry> &out, std::set<std::string> &seen) {
  for (auto &n : list_dir(dir, true)) {
    std::string p = path_join(dir, n);
    if (is_dir(p)) {
      scan_apps(base, p, prefix + n + "-", out, seen);
      continue;
    }
    if (!ends_with(n, ".desktop")) continue;
    std::string id = prefix + n;
    if (seen.count(id)) continue; // earlier data dir wins
    seen.insert(id);
    DesktopEntry e;
    if (e.load(p, id) && e.type == "Application") out[id] = e;
  }
}

std::vector<DesktopEntry> all_applications(bool include_hidden) {
  std::map<std::string, DesktopEntry> m;
  std::set<std::string> seen;
  for (auto &d : data_dirs()) {
    std::string a = path_join(d, "applications");
    scan_apps(a, a, "", m, seen);
  }
  std::vector<DesktopEntry> v;
  for (auto &kv : m)
    if (include_hidden || kv.second.visible()) v.push_back(kv.second);
  std::sort(v.begin(), v.end(), [](const DesktopEntry &a, const DesktopEntry &b) {
    return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
  });
  return v;
}

bool find_application(const std::string &id_in, DesktopEntry &out) {
  std::string id = ends_with(id_in, ".desktop") ? id_in : id_in + ".desktop";
  for (auto &d : data_dirs()) {
    std::string a = path_join(d, "applications");
    std::string p = path_join(a, id);
    if (file_exists(p) && out.load(p, id)) return true;
    // id "foo-bar.desktop" may live in applications/foo/bar.desktop
    std::string sub = id;
    for (size_t i = 0; i < sub.size(); i++) {
      if (sub[i] != '-') continue;
      std::string cand = path_join(a, sub.substr(0, i) + "/" + sub.substr(i + 1));
      if (file_exists(cand) && out.load(cand, id)) return true;
    }
  }
  return false;
}

} // namespace fld
