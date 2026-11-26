// fl-settings: Startup Applications (autostart_model) and the command line (settings_cli)
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "autostart_model.h"
#include "fld/ini.h"
#include "fld/util.h"
#include "settings_cli.h"
#include "test.h"

int main() {
  char tmpl[] = "/tmp/fld-autostart-XXXXXX";
  std::string tmp = mkdtemp(tmpl);
  std::string user = tmp + "/user", sys = tmp + "/sys";
  fld::mkdir_p(user + "/autostart");
  fld::mkdir_p(sys + "/autostart");
  setenv("XDG_CURRENT_DESKTOP", "fldesktop", 1);
  fld::write_file(sys + "/autostart/agent.desktop",
                  "[Desktop Entry]\nType=Application\nName=Agent\nExec=agent --daemon\n");
  fld::write_file(sys + "/autostart/gnome-only.desktop",
                  "[Desktop Entry]\nType=Application\nName=Gnome thing\nExec=g\nOnlyShowIn=GNOME;\n");
  std::vector<std::string> dirs = {user, sys};
  auto l = autostart::list(dirs);
  CHECK_EQ(l.size(), (size_t)1); // not the GNOME-only one
  CHECK_EQ(l[0].name, std::string("Agent"));
  CHECK(l[0].enabled);
  CHECK(l[0].user_path.empty());

  // turning a system entry off makes a user copy with Hidden=true
  std::string err;
  CHECK(autostart::set_enabled(l[0], false, user + "/autostart", err));
  l = autostart::list(dirs);
  CHECK(!l[0].enabled);
  CHECK(!l[0].user_path.empty());
  fld::Ini ini;
  ini.load(user + "/autostart/agent.desktop");
  CHECK(ini.get_bool("Desktop Entry", "Hidden", false));
  CHECK_EQ(ini.get("Desktop Entry", "Exec"), std::string("agent --daemon"));
  // and on again: the copy goes away
  CHECK(autostart::set_enabled(l[0], true, user + "/autostart", err));
  CHECK(!fld::file_exists(user + "/autostart/agent.desktop"));
  l = autostart::list(dirs);
  CHECK(l[0].enabled);

  // a new entry, then removed
  std::string id = autostart::add(user + "/autostart", "My Tool 2", "mytool --x", "", "", err);
  CHECK_EQ(id, std::string("my-tool-2.desktop"));
  CHECK_EQ(autostart::add(user + "/autostart", "My Tool 2", "other", "", "", err), std::string("my-tool-2-2.desktop"));
  CHECK(autostart::add(user + "/autostart", "x", "a\nb", "", "", err).empty());
  l = autostart::list(dirs);
  CHECK_EQ(l.size(), (size_t)3);
  for (auto &e : l)
    if (e.id == id) CHECK(autostart::remove(e, user + "/autostart", err));
  CHECK(!fld::file_exists(user + "/autostart/" + id));
  // removing a system entry turns it off
  l = autostart::list(dirs);
  for (auto &e : l)
    if (e.id == "agent.desktop") CHECK(autostart::remove(e, user + "/autostart", err));
  CHECK(fld::file_exists(sys + "/autostart/agent.desktop"));
  for (auto &e : autostart::list(dirs))
    if (e.id == "agent.desktop") CHECK(!e.enabled);
  CHECK_EQ(autostart::id_for("  Hello, World! "), std::string("hello-world"));
  CHECK_EQ(autostart::id_for("!!"), std::string("program"));

  // the command line
  settings_cli::Key k;
  CHECK(settings_cli::parse_key("wm.behaviour.workspaces", "/c", k, err));
  CHECK_EQ(k.file, std::string("/c/wm.ini"));
  CHECK_EQ(k.section, std::string("behaviour"));
  CHECK_EQ(k.key, std::string("workspaces"));
  CHECK(settings_cli::parse_key("desktop.Night.Light.Temperature", "/c", k, err));
  CHECK_EQ(k.section, std::string("Night.Light"));
  CHECK(!settings_cli::parse_key("wm.workspaces", "/c", k, err));
  CHECK(!settings_cli::parse_key("../x.a.b", "/c", k, err));
  CHECK(!settings_cli::parse_key(".a.b", "/c", k, err));
  std::string cfg = tmp + "/fldesktop";
  unsetenv("FLWM_PID"); // no reload
  char a0[] = "--set", a1[] = "panel.Clock.Format=%H:%M";
  char *set_args[] = {a0, a1};
  CHECK_EQ(settings_cli::run(2, set_args, cfg), 0);
  fld::Ini p;
  p.load(cfg + "/panel.ini");
  CHECK_EQ(p.get("Clock", "Format"), std::string("%H:%M"));
  char g0[] = "--get", g1[] = "panel.Clock.Nope";
  char *get_args[] = {g0, g1};
  CHECK_EQ(settings_cli::run(2, get_args, cfg), 1);
  char u0[] = "--unset", u1[] = "panel.Clock.Format";
  char *unset_args[] = {u0, u1};
  CHECK_EQ(settings_cli::run(2, unset_args, cfg), 0);
  p.load(cfg + "/panel.ini");
  CHECK(!p.has("Clock", "Format"));
  char o0[] = "--open";
  char *other[] = {o0};
  CHECK_EQ(settings_cli::run(1, other, cfg), -1);
  CHECK(settings_cli::safe_members("fldesktop/\nfldesktop/wm.ini\n", err));
  CHECK(!settings_cli::safe_members("fldesktop/../../.bashrc\n", err));
  CHECK(!settings_cli::safe_members("/etc/passwd\n", err));
  CHECK(!settings_cli::safe_members("other/wm.ini\n", err));
  CHECK(!settings_cli::safe_members("", err));
  // export and import round trip
  if (!fld::which("tar").empty()) {
    std::string arch = tmp + "/s.tar.gz";
    std::string arch_s = arch;
    char e0[] = "--export";
    char *exp[] = {e0, &arch_s[0]};
    CHECK_EQ(settings_cli::run(2, exp, cfg), 0);
    fld::write_file(cfg + "/panel.ini", "[Clock]\nFormat=changed\n");
    char i0[] = "--import";
    char *imp[] = {i0, &arch_s[0]};
    CHECK_EQ(settings_cli::run(2, imp, cfg), 0);
    p.load(cfg + "/panel.ini");
    CHECK(!p.has("Clock", "Format"));
  }
  std::string out;
  fld::run_capture({"rm", "-rf", tmp}, out);
  return test_result("settings-autostart");
}
