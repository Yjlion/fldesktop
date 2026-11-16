// fl-autostart: starts the XDG autostart entries (~/.config/autostart, /etc/xdg/autostart)
#include <cstdio>
#include <map>
#include <string>

#include "fld/desktop_entry.h"
#include "fld/util.h"

int main(int argc, char **argv) {
  bool dry = argc > 1 && std::string(argv[1]) == "--list";
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
