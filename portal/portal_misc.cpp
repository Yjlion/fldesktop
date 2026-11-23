// Pure helpers of xdg-desktop-portal-fl: autostart entries (Background) and
// shortcut triggers and their store (GlobalShortcuts). Unit tested.
#include <cctype>
#include <cstring>

#include "fld/ini.h"
#include "fld/util.h"
#include "portal.h"

namespace portal {

namespace {

bool safe_id(const std::string &id) {
  if (id.empty() || id.size() > 255 || id[0] == '.' || id[0] == '-') return false;
  for (char c : id)
    if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
  return true;
}

// one Exec argument (Desktop Entry Specification: quoting and %%)
std::string exec_arg(const std::string &a) {
  bool quote = a.empty();
  for (char c : a)
    if (strchr(" \t\n\"'\\><~|&;$*?#()`", c)) quote = true;
  std::string o;
  for (char c : a) {
    if (c == '%') {
      o += "%%";
      continue;
    }
    if (quote && (c == '"' || c == '`' || c == '$' || c == '\\')) o += '\\';
    o += c;
  }
  return quote ? "\"" + o + "\"" : o;
}

} // namespace

std::string autostart_file_name(const std::string &app_id) { return safe_id(app_id) ? app_id + ".desktop" : ""; }

std::string autostart_entry(const std::string &app_id, const std::vector<std::string> &argv, unsigned flags) {
  std::string exec;
  for (const std::string &a : argv) exec += (exec.empty() ? "" : " ") + exec_arg(a);
  std::string e = "[Desktop Entry]\nType=Application\nName=" + app_id + "\nExec=" + exec + "\n";
  e += "X-XDP-Autostart=" + app_id + "\n";
  if (flags & 1) e += "DBusActivatable=true\n"; // AUTOSTART_FLAGS_ACTIVATABLE
  return e;
}

// "CTRL+ALT+k", "<Control><Alt>k" -> "Ctrl+Alt+k"; "" when there is no key
std::string trigger_to_combo(const std::string &trigger) {
  std::string t = fld::trim(trigger);
  std::vector<std::string> mods;
  std::string key;
  auto add_mod = [&](std::string m) {
    m = fld::lower(m);
    if (m == "ctrl" || m == "control" || m == "primary") mods.push_back("Ctrl");
    else if (m == "alt" || m == "mod1") mods.push_back("Alt");
    else if (m == "shift") mods.push_back("Shift");
    else if (m == "logo" || m == "super" || m == "meta" || m == "mod4") mods.push_back("Super");
    else return false; // NUM and others: no such modifier for bindings
    return true;
  };
  if (!t.empty() && t[0] == '<') {
    // GTK accelerator syntax
    size_t i = 0;
    while (i < t.size() && t[i] == '<') {
      size_t e = t.find('>', i);
      if (e == std::string::npos) return "";
      if (!add_mod(t.substr(i + 1, e - i - 1))) return "";
      i = e + 1;
    }
    key = t.substr(i);
  } else {
    std::vector<std::string> parts = fld::split(t, '+');
    for (size_t i = 0; i < parts.size(); i++) {
      std::string p = fld::trim(parts[i]);
      if (i + 1 == parts.size()) key = p;
      else if (!add_mod(p)) return "";
    }
  }
  if (key.empty() || key.find_first_of(" \t=[]") != std::string::npos) return "";
  // "CTRL+" names no key
  std::string lk = fld::lower(key);
  for (const char *m : {"ctrl", "control", "alt", "shift", "logo", "super", "meta", "num"})
    if (lk == m) return "";
  // order: Super, Ctrl, Alt, Shift
  std::string out;
  for (const char *m : {"Super", "Ctrl", "Alt", "Shift"})
    for (const std::string &x : mods)
      if (x == m) {
        out += x + "+";
        break;
      }
  return out + key;
}

std::string shortcuts_path() { return fld::fld_config_dir() + "/global-shortcuts.ini"; }

std::vector<StoredShortcut> shortcuts_load(const std::string &path, const std::string &app_id) {
  fld::Ini ini;
  ini.load(path);
  std::vector<StoredShortcut> out;
  for (const std::string &k : ini.keys(app_id)) {
    std::string v = ini.get(app_id, k, "");
    size_t bar = v.find('|');
    StoredShortcut s;
    s.id = k;
    s.combo = fld::trim(v.substr(0, bar));
    s.description = bar == std::string::npos ? "" : v.substr(bar + 1);
    out.push_back(s);
  }
  return out;
}

void shortcuts_save(const std::string &path, const std::string &app_id, const std::vector<StoredShortcut> &list) {
  fld::Ini ini;
  ini.load(path);
  ini.remove_group(app_id);
  for (const StoredShortcut &s : list)
    if (!s.id.empty() && s.id.find_first_of("=[]\n\r") == std::string::npos && s.id[0] != '#' && s.id[0] != ';')
      ini.set(app_id, s.id, s.combo + "|" + s.description);
  fld::mkdir_p(fld::dirname(path), 0755);
  ini.save(path);
}

} // namespace portal
