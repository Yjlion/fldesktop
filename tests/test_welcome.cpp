// fl-welcome: layouts, wallpapers, accents
#include <cstdlib>

#include "fld/util.h"
#include "test.h"
#include "welcome_util.h"

using namespace welcome;

int main() {
  std::string l, v;
  layout_code("fr(bepo)", l, v);
  CHECK(l == "fr" && v == "bepo");
  layout_code("de", l, v);
  CHECK(l == "de" && v.empty());
  CHECK_EQ(layout_from_locale("de_DE.UTF-8"), std::string("de"));
  CHECK_EQ(layout_from_locale("en_GB.UTF-8"), std::string("gb"));
  CHECK_EQ(layout_from_locale("en_US.UTF-8"), std::string("us"));
  CHECK_EQ(layout_from_locale("pt_BR.UTF-8"), std::string("br"));
  CHECK_EQ(layout_from_locale("sv_SE.UTF-8"), std::string("se"));
  CHECK_EQ(layout_from_locale("C.UTF-8"), std::string("us"));
  CHECK_EQ(layout_from_locale("fr_CH"), std::string("ch(fr)"));
  CHECK(accents().size() >= 6);
  for (auto &a : accents()) CHECK(a.second.size() == 7 && a.second[0] == '#');
  CHECK_EQ(flathub_command()[0], std::string("flatpak"));
  CHECK_EQ(done_marker("/c"), std::string("/c/welcome-done"));
  char tmpl[] = "/tmp/fld-welcome-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  fld::mkdir_p(dir + "/a");
  fld::mkdir_p(dir + "/b");
  fld::write_file(dir + "/a/z.png", "x");
  fld::write_file(dir + "/b/m.jpg", "x");
  fld::write_file(dir + "/b/notes.txt", "x");
  auto w = wallpapers({dir + "/a", dir + "/b", dir + "/none"});
  CHECK_EQ(w.size(), (size_t)2);
  CHECK_EQ(fld::basename(w[0]), std::string("m.jpg"));
  std::string out;
  fld::run_capture({"rm", "-r", dir}, out);
  return test_result("welcome");
}
