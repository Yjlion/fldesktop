// fl-panel applet plugins: the loader (shell/applet_loader.cpp) with the
// example applet, one built for another ABI version, and a library that is
// not an applet. usage: test_applet_plugins <dir with the modules>
#include <FL/Fl_Group.H>
#include <FL/Fl_Widget.H>
#include <cstdlib>
#include <string>

#include "applet_loader.h"
#include "fld/util.h"
#include "test.h"

static int relayouts = 0;
static const char *config(const char *id, const char *key, const char *def) {
  return std::string(id) == "uptime" && std::string(key) == "Prefix" ? "since" : def;
}
static void spawn(const char *const *) {}

int main(int argc, char **argv) {
  CHECK(argc == 2);
  if (argc != 2) return 1;
  std::string dir = argv[1];
  fld_applet_host host{FLD_APPLET_ABI_VERSION, 30, 0, 0, 0, [] { relayouts++; }, config, spawn};

  CHECK(applets::valid_id("uptime"));
  CHECK(applets::valid_id("cpu-temp2"));
  CHECK(!applets::valid_id(""));
  CHECK(!applets::valid_id("Up time"));
  CHECK(!applets::valid_id("../x"));
  CHECK(!applets::valid_id(nullptr));

  setenv("FLD_APPLET_PATH", (dir + ":/nonexistent").c_str(), 1);
  auto dirs = applets::search_dirs("/usr/lib");
  CHECK(dirs.size() == 4);
  CHECK_EQ(dirs[0], dir);
  CHECK_EQ(dirs.back(), std::string("/usr/lib/fldesktop/applets"));

  Fl_Group g(0, 0, 400, 30);
  std::vector<std::string> errors;
  auto loaded = applets::load({dir}, {}, host, &errors);
  g.end();
  // uptime.so loads; old-abi.so (ABI 999) and not-an-applet.so are refused
  CHECK_EQ(loaded.size(), (size_t)1);
  if (!loaded.empty()) {
    CHECK_EQ(loaded[0].id, std::string("uptime"));
    CHECK_EQ(loaded[0].name, std::string("Uptime"));
    Fl_Widget *w = (Fl_Widget *)loaded[0].widget;
    CHECK(w != nullptr);
    CHECK(w->parent() == &g);  // made in the current group
    CHECK_EQ(w->h(), 30);
    CHECK(w->label() && std::string(w->label()).rfind("since ", 0) == 0);  // [Applet:uptime] Prefix
  }
  CHECK_EQ(errors.size(), (size_t)2);
  bool abi_msg = false, entry_msg = false;
  for (auto &e : errors) {
    if (e.find("ABI 999") != std::string::npos) abi_msg = true;
    if (e.find("no fld_applet_v1") != std::string::npos) entry_msg = true;
  }
  CHECK(abi_msg);
  CHECK(entry_msg);

  // disabled, and the same id in a later directory
  Fl_Group g2(0, 0, 400, 30);
  auto none = applets::load({dir}, {"uptime"}, host, nullptr);
  auto once = applets::load({dir, dir}, {}, host, nullptr);
  g2.end();
  CHECK(none.empty());
  CHECK_EQ(once.size(), (size_t)1);
  return test_result("applet-plugins");
}
