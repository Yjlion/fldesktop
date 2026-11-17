#include "permissions.h"

#include <algorithm>

#include "fld/ini.h"
#include "fld/util.h"

namespace fp {

const std::vector<PermItem> &perm_items() {
  static const std::vector<PermItem> items = {
      {"shared", "network", "Network access"},
      {"shared", "ipc", "Inter-process communication with the host"},
      {"sockets", "wayland", "Wayland windowing system"},
      {"sockets", "x11", "X11 windowing system"},
      {"sockets", "fallback-x11", "X11 when Wayland is not available"},
      {"sockets", "pulseaudio", "Sound (PulseAudio / PipeWire)"},
      {"sockets", "session-bus", "All of the session bus"},
      {"sockets", "system-bus", "All of the system bus"},
      {"sockets", "ssh-auth", "SSH agent"},
      {"sockets", "cups", "Printing system (CUPS)"},
      {"sockets", "pcsc", "Smart cards"},
      {"sockets", "gpg-agent", "GPG agent"},
      {"devices", "dri", "GPU acceleration"},
      {"devices", "input", "Input devices (joysticks)"},
      {"devices", "usb", "USB devices"},
      {"devices", "kvm", "Virtualization (KVM)"},
      {"devices", "shm", "Shared memory"},
      {"devices", "all", "All devices (webcams, game pads...)"},
      {"features", "bluetooth", "Bluetooth"},
      {"features", "devel", "Development syscalls (debuggers)"},
      {"features", "multiarch", "Programs of other architectures"},
      {"features", "canbus", "CAN bus sockets"},
      {"features", "per-app-dev-shm", "Own /dev/shm"},
  };
  return items;
}

namespace {

void parse_list(const std::string &v, std::map<std::string, bool> &out) {
  for (auto &x : fld::split(v, ';')) {
    std::string t = fld::trim(x);
    if (t.empty()) continue;
    if (t[0] == '!') out[t.substr(1)] = false;
    else out[t] = true;
  }
}

const char *kCats[] = {"shared", "sockets", "devices", "features"};

} // namespace

bool Permissions::effective(const std::string &cat, const std::string &key) const {
  auto oc = over.find(cat);
  if (oc != over.end()) {
    auto ok = oc->second.find(key);
    if (ok != oc->second.end()) return ok->second;
  }
  auto ac = app.find(cat);
  if (ac != app.end()) {
    auto ak = ac->second.find(key);
    if (ak != ac->second.end()) return ak->second;
  }
  return false;
}

bool Permissions::overridden(const std::string &cat, const std::string &key) const {
  auto oc = over.find(cat);
  return oc != over.end() && oc->second.count(key);
}

void Permissions::set(const std::string &cat, const std::string &key, bool on) {
  bool def = false;
  auto ac = app.find(cat);
  if (ac != app.end() && ac->second.count(key)) def = ac->second.at(key);
  if (on == def) over[cat].erase(key);
  else over[cat][key] = on;
}

static std::string fs_base(const std::string &fs) {
  size_t c = fs.rfind(':');
  if (c != std::string::npos && (fs.substr(c) == ":ro" || fs.substr(c) == ":rw" || fs.substr(c) == ":create"))
    return fs.substr(0, c);
  return fs;
}

std::vector<std::string> Permissions::filesystems() const {
  std::vector<std::string> out;
  for (auto &f : app_filesystems) {
    auto it = fs_over.find(fs_base(f));
    if (it != fs_over.end() && !it->second) continue; // revoked
    out.push_back(f);
  }
  for (auto &kv : fs_over) {
    if (!kv.second) continue;
    bool have = false;
    for (auto &o : out)
      if (fs_base(o) == fs_base(kv.first)) have = true;
    if (!have) out.push_back(kv.first);
  }
  return out;
}

void Permissions::add_filesystem(const std::string &fs) {
  std::string base = fs_base(fs);
  // re-granting one of the app's own: just drop the revocation
  for (auto &f : app_filesystems)
    if (fs_base(f) == base && f == fs) {
      fs_over.erase(base);
      return;
    }
  for (auto it = fs_over.begin(); it != fs_over.end();)
    it = fs_base(it->first) == base ? fs_over.erase(it) : std::next(it);
  fs_over[fs] = true;
}

void Permissions::remove_filesystem(const std::string &fs) {
  std::string base = fs_base(fs);
  for (auto it = fs_over.begin(); it != fs_over.end();)
    it = fs_base(it->first) == base ? fs_over.erase(it) : std::next(it);
  for (auto &f : app_filesystems)
    if (fs_base(f) == base) fs_over[base] = false;
}

void Permissions::reset() {
  over.clear();
  fs_over.clear();
  env_over.clear();
}

void parse_metadata(const std::string &text, Permissions &p) {
  fld::Ini ini;
  ini.parse(text);
  for (const char *c : kCats) parse_list(ini.get("Context", c), p.app[c]);
  p.app_filesystems.clear();
  for (auto &x : fld::split(ini.get("Context", "filesystems"), ';'))
    if (!fld::trim(x).empty() && fld::trim(x)[0] != '!') p.app_filesystems.push_back(fld::trim(x));
  for (auto &k : ini.keys("Environment")) p.app_env[k] = ini.get("Environment", k);
}

void parse_overrides(const std::string &text, Permissions &p) {
  fld::Ini ini;
  ini.parse(text);
  for (const char *c : kCats) parse_list(ini.get("Context", c), p.over[c]);
  p.fs_over.clear();
  parse_list(ini.get("Context", "filesystems"), p.fs_over);
  for (auto &k : ini.keys("Environment")) p.env_over[k] = ini.get("Environment", k);
}

std::string write_overrides(const std::string &existing, const Permissions &p) {
  fld::Ini ini;
  ini.parse(existing);
  auto join = [](const std::map<std::string, bool> &m) {
    std::string s;
    for (auto &kv : m) s += (kv.second ? "" : "!") + kv.first + ";";
    return s;
  };
  for (const char *c : kCats) {
    auto it = p.over.find(c);
    std::string v = it == p.over.end() ? "" : join(it->second);
    if (v.empty()) ini.remove("Context", c);
    else ini.set("Context", c, v);
  }
  std::string fs = join(p.fs_over);
  if (fs.empty()) ini.remove("Context", "filesystems");
  else ini.set("Context", "filesystems", fs);
  if (ini.keys("Context").empty()) ini.remove_group("Context");
  for (auto &k : ini.keys("Environment"))
    if (!p.env_over.count(k)) ini.remove("Environment", k);
  for (auto &kv : p.env_over) ini.set("Environment", kv.first, kv.second);
  if (ini.keys("Environment").empty()) ini.remove_group("Environment");
  return ini.to_string();
}

std::string filesystem_label(const std::string &fs) {
  std::string base = fs_base(fs), mode;
  if (fs.size() > base.size()) mode = fs.substr(base.size() + 1);
  static const std::pair<const char *, const char *> names[] = {
      {"host", "All files"},
      {"host-os", "System libraries and programs"},
      {"host-etc", "System configuration"},
      {"home", "Home folder"},
      {"xdg-desktop", "Desktop folder"},
      {"xdg-documents", "Documents folder"},
      {"xdg-download", "Downloads folder"},
      {"xdg-music", "Music folder"},
      {"xdg-pictures", "Pictures folder"},
      {"xdg-videos", "Videos folder"},
      {"xdg-public-share", "Public folder"},
      {"xdg-templates", "Templates folder"},
      {"xdg-config", "Configuration (~/.config)"},
      {"xdg-cache", "Cache (~/.cache)"},
      {"xdg-data", "Data (~/.local/share)"},
      {"xdg-run", "Runtime folder"},
  };
  std::string label = base;
  std::string head = base, tail;
  size_t slash = base.find('/');
  if (slash != std::string::npos) {
    head = base.substr(0, slash);
    tail = base.substr(slash);
  }
  for (auto &n : names)
    if (head == n.first) label = std::string(n.second) + (tail.empty() ? "" : " " + tail.substr(1));
  if (mode == "ro") label += " (read only)";
  else if (mode == "create") label += " (created if needed)";
  return label;
}

} // namespace fp
