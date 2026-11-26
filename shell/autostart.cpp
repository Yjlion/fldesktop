// fl-autostart: starts the XDG autostart entries (~/.config/autostart, /etc/xdg/autostart)
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <unistd.h>

#include "fld/toolkit_theme.h"
#include "fld/desktop_entry.h"
#include "fld/ini.h"
#include "fld/util.h"

// a process named `comm` runs (as anyone)
static bool running(const std::string &comm) {
  for (auto &pid : fld::list_dir("/proc")) {
    if (pid.empty() || pid[0] < '0' || pid[0] > '9') continue;
    std::string c;
    if (fld::read_file("/proc/" + pid + "/comm", c, 64) && fld::trim(c) == comm) return true;
  }
  return false;
}

// Without systemd (Alpine, Devuan) nothing socket-activates PipeWire: start it
// with the session bus here unless an autostart entry does
static void start_pipewire(const std::map<std::string, std::string> &entries, bool dry) {
  if (fld::is_dir("/run/systemd/system") || running("pipewire")) return;
  for (auto &kv : entries)
    if (fld::starts_with(kv.first, "pipewire")) return;
  std::vector<std::vector<std::string>> cmds;
  for (const char *l : {"/usr/libexec/pipewire-launcher", "/usr/lib/pipewire/pipewire-launcher"})
    if (access(l, X_OK) == 0) { cmds.push_back({l}); break; }
  if (cmds.empty() && !fld::which("pipewire").empty()) {
    cmds.push_back({"pipewire"});
    if (!fld::which("wireplumber").empty()) cmds.push_back({"wireplumber"});
    if (!fld::which("pipewire-pulse").empty()) cmds.push_back({"pipewire-pulse"});
  }
  for (auto &c : cmds) {
    if (dry) printf("pipewire\t%s\n", c[0].c_str());
    else fld::spawn(c);
  }
}

// the programs of the last session ([behaviour] restore_session in wm.ini):
// fl-wm wrote their app ids to session.ini; fl-wm puts their windows back
static void restore_session(const std::set<std::string> &started, bool dry) {
  fld::Ini wm;
  wm.load(fld::wm_config_path());
  if (!wm.get_bool("behaviour", "restore_session", false)) return;
  std::string state = fld::getenv_or("XDG_STATE_HOME", "");
  if (state.empty()) state = fld::home_dir() + "/.local/state";
  fld::Ini s;
  if (!s.load(state + "/fldesktop/session.ini")) return;
  std::set<std::string> done;
  for (auto &g : s.groups()) {
    std::string id = s.get(g, "app_id");
    if (id.empty() || done.count(id) || started.count(id) || fld::starts_with(id, "fldesktop.")) continue;
    done.insert(id);
    fld::DesktopEntry e;
    // app ids are usually the desktop file's name (org.example.App, fl-term)
    if (!fld::find_application(id, e) && !fld::find_application(fld::lower(id), e)) {
      bool found = false;
      for (auto &a : fld::all_applications())
        if (fld::basename(fld::shell_split(a.exec).empty() ? "" : fld::shell_split(a.exec)[0]) == id) {
          e = a;
          found = true;
          break;
        }
      if (!found) continue;
    }
    if (dry) printf("session\t%s\t%s\n", id.c_str(), e.exec.c_str());
    else e.launch();
  }
}

int main(int argc, char **argv) {
  bool dry = argc > 1 && std::string(argv[1]) == "--list";
  if (!dry && !fld::which("dbus-update-activation-environment").empty()) {
    // programs started by D-Bus (xdg-desktop-portal-fl, fl-notifyd) need the
    // session's display (and fl-session's libdecor setting); also tell
    // systemd --user when there is one
    std::vector<std::string> vars = {"WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE",
                                     "XDG_SESSION_DESKTOP", "DISPLAY", "LIBDECOR_PLUGIN_DIR"};
    std::vector<std::string> cmd = {"dbus-update-activation-environment", "--systemd"};
    for (auto &v : vars)
      if (getenv(v.c_str())) cmd.push_back(v);
    std::string out;
    if (fld::run_capture(cmd, out) != 0) {
      cmd.erase(cmd.begin() + 1);
      fld::run_capture(cmd, out);
    }
  }
  // GTK and Qt programs get the desktop's colours, also after desktop.ini
  // was edited by hand or by an older version
  if (!dry) fld::export_toolkit_theme();
  // fl-wm could not use the graphics card (fl-wm or fl-session fell back)
  std::string sw = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fldesktop-software-rendering";
  if (!dry && fld::file_exists(sw) && !fld::which("notify-send").empty()) {
    unlink(sw.c_str());
    fld::spawn({"notify-send", "-a", "fldesktop", "-i", "video-display",
                "Software rendering",
                "The graphics driver could not be used, so the desktop is drawn by the processor. "
                "It works, but slower. System Info > Graphics shows the details."});
  }
  std::map<std::string, std::string> entries; // id -> path, user config wins
  auto dirs = fld::config_dirs();
  for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
    std::string d = fld::path_join(*it, "autostart");
    for (auto &n : fld::list_dir(d, true))
      if (fld::ends_with(n, ".desktop")) entries[n] = fld::path_join(d, n);
  }
  start_pipewire(entries, dry);
  std::set<std::string> started; // app ids the autostart entries start
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
  for (auto &kv : entries) started.insert(kv.first.substr(0, kv.first.size() - 8));
  restore_session(started, dry);
  return 0;
}
