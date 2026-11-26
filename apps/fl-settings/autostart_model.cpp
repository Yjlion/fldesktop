#include "autostart_model.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <map>
#include <unistd.h>

#include "fld/desktop_entry.h"
#include "fld/ini.h"
#include "fld/util.h"

namespace autostart {

static const char *const G = "Desktop Entry";

std::vector<Entry> list(const std::vector<std::string> &dirs) {
  std::map<std::string, Entry> by_id;
  // system dirs first, the user's last: the user's copy wins
  for (size_t i = dirs.size(); i-- > 0;) {
    std::string d = fld::path_join(dirs[i], "autostart");
    for (auto &n : fld::list_dir(d, true)) {
      if (!fld::ends_with(n, ".desktop")) continue;
      std::string path = fld::path_join(d, n);
      fld::DesktopEntry de;
      if (!de.load(path, n)) continue;
      Entry &e = by_id[n];
      e.id = n;
      if (i == 0) e.user_path = path;
      else e.system_path = path;
      // a user file that only hides a system one has no name of its own
      if (!de.name.empty() || e.name.empty()) e.name = de.name;
      if (!de.comment.empty()) e.comment = de.comment;
      if (!de.exec.empty()) e.exec = de.exec;
      if (!de.icon.empty()) e.icon = de.icon;
      e.enabled = !de.hidden;
      // for this desktop? (visible() without Hidden and NoDisplay)
      de.hidden = false;
      de.no_display = false;
      if (!de.visible()) { by_id.erase(n); continue; }
    }
  }
  std::vector<Entry> out;
  for (auto &kv : by_id) {
    Entry e = kv.second;
    if (e.name.empty()) e.name = e.id.substr(0, e.id.size() - 8);
    out.push_back(e);
  }
  std::sort(out.begin(), out.end(), [](const Entry &a, const Entry &b) {
    return fld::lower(a.name) < fld::lower(b.name);
  });
  return out;
}

static bool write_ini(const fld::Ini &ini, const std::string &user_dir, const std::string &id, std::string &err) {
  fld::mkdir_p(user_dir);
  std::string path = fld::path_join(user_dir, id);
  if (!ini.save(path)) {
    err = "Cannot write " + path + ": " + strerror(errno);
    return false;
  }
  return true;
}

bool set_enabled(const Entry &e, bool on, const std::string &user_dir, std::string &err) {
  fld::Ini ini;
  if (!e.user_path.empty()) ini.load(e.user_path);
  else if (!e.system_path.empty()) ini.load(e.system_path);
  else {
    err = "No such entry: " + e.id;
    return false;
  }
  // a copy that only hid the system entry: back to the system file
  if (on && !e.user_path.empty() && !e.system_path.empty()) {
    fld::Ini sys;
    sys.load(e.system_path);
    ini.remove(G, "Hidden");
    sys.remove(G, "Hidden");
    if (ini.to_string() == sys.to_string() || ini.keys(G).empty()) {
      if (unlink(e.user_path.c_str()) != 0 && errno != ENOENT) {
        err = "Cannot remove " + e.user_path + ": " + strerror(errno);
        return false;
      }
      return true;
    }
  }
  if (on) ini.remove(G, "Hidden");
  else ini.set_bool(G, "Hidden", true);
  return write_ini(ini, user_dir, e.id, err);
}

std::string id_for(const std::string &name) {
  std::string id;
  for (unsigned char c : name) {
    if (std::isalnum(c)) id += (char)std::tolower(c);
    else if (!id.empty() && id.back() != '-') id += '-';
  }
  while (!id.empty() && id.back() == '-') id.pop_back();
  return id.empty() ? "program" : id;
}

std::string add(const std::string &user_dir, const std::string &name, const std::string &exec,
                const std::string &icon, const std::string &comment, std::string &err) {
  std::string n = fld::trim(name), x = fld::trim(exec);
  if (n.empty() || x.empty()) {
    err = "A name and a command are needed.";
    return "";
  }
  if (n.find('\n') != std::string::npos || x.find('\n') != std::string::npos) {
    err = "The name and the command must be on one line.";
    return "";
  }
  std::string base = id_for(n), id = base + ".desktop";
  for (int i = 2; fld::file_exists(fld::path_join(user_dir, id)); i++) id = base + "-" + std::to_string(i) + ".desktop";
  fld::Ini ini;
  ini.set(G, "Type", "Application");
  ini.set(G, "Name", n);
  ini.set(G, "Exec", x);
  if (!icon.empty()) ini.set(G, "Icon", icon);
  if (!comment.empty()) ini.set(G, "Comment", comment);
  return write_ini(ini, user_dir, id, err) ? id : "";
}

bool remove(const Entry &e, const std::string &user_dir, std::string &err) {
  if (!e.system_path.empty()) return set_enabled(e, false, user_dir, err);
  if (e.user_path.empty()) return true;
  if (unlink(e.user_path.c_str()) != 0 && errno != ENOENT) {
    err = "Cannot remove " + e.user_path + ": " + strerror(errno);
    return false;
  }
  return true;
}

} // namespace autostart
