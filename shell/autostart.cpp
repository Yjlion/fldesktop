// fl-autostart: starts the XDG autostart entries (~/.config/autostart, /etc/xdg/autostart)
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <string>

#include "fld/desktop_entry.h"
#include "fld/util.h"

int main(int argc, char **argv) {
  bool dry = argc > 1 && std::string(argv[1]) == "--list";
  if (!dry && !fld::which("dbus-update-activation-environment").empty()) {
    // programs started by D-Bus (xdg-desktop-portal-fl, fl-notifyd) need the
    // session's display; also tell systemd --user when there is one
    std::vector<std::string> vars = {"WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
                                     "XDG_SESSION_DESKTOP", "DISPLAY"};
    std::vector<std::string> cmd = {"dbus-update-activation-environment", "--systemd"};
    for (auto &v : vars)
      if (getenv(v.c_str())) cmd.push_back(v);
    std::string out;
    if (fld::run_capture(cmd, out) != 0) {
      cmd.erase(cmd.begin() + 1);
      fld::run_capture(cmd, out);
    }
  }
  std::map<std::string, std::string> entries; // id -> path, user config wins
  auto dirs = fld::config_dirs();
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
    std::string d = fld::path_join(*it, "autostart");
    for (auto &n : fld::list_dir(d, true))
      if (fld::ends_with(n, ".desktop")) entries[n] = fld::path_join(d, n);
  }
  for (auto &kv : entries) {
    fld::DesktopEntry e;
    if (!e.load(kv.second, kv.first)) continue;
    // NoDisplay does not matter for autostart; Hidden means "deleted"
    e.no_display = false;
    if (!e.visible() || e.exec.empty()) continue;
    if (dry) {
      printf("%s\t%s\n", kv.first.c_str(), e.exec.c_str());
      continue;
    }
    if (!e.launch()) fprintf(stderr, "fl-autostart: failed to start %s\n", kv.first.c_str());
  }
  return 0;
}
