// fl-fonts: families, user fonts, install paths
#include <cstdlib>

#include "fld/util.h"
#include "fonts_util.h"
#include "test.h"

using namespace fonts;

int main() {
  std::string home = "/home/u";
  std::vector<Face> faces = {
      {"DejaVu Sans,DejaVu Sans Condensed", "Bold", "/usr/share/fonts/DejaVuSans-Bold.ttf", 200, 0},
      {"DejaVu Sans", "Book", "/usr/share/fonts/DejaVuSans.ttf", 80, 0},
      {"DejaVu Sans", "Oblique", "/usr/share/fonts/DejaVuSans-Oblique.ttf", 80, 110},
      {"Fira Code", "Regular", "/home/u/.local/share/fonts/FiraCode.ttf", 80, 0},
      {"Fira Code", "Regular", "/home/u/.local/share/fonts/FiraCode.woff2", 80, 0},
      {"abc", "Regular", "/home/u/.fonts/abc.otf", 80, 0},
  };
  auto g = group(faces, home);
  CHECK_EQ(g.size(), (size_t)3);
  CHECK_EQ(g[0].name, std::string("abc"));
  CHECK(g[0].user);
  CHECK_EQ(g[1].name, std::string("DejaVu Sans"));
  CHECK(!g[1].user);
  CHECK_EQ(g[1].faces.size(), (size_t)3);
  CHECK_EQ(g[1].faces[0].style, std::string("Book")); // regular first
  CHECK_EQ(g[1].faces[1].style, std::string("Bold"));
  CHECK_EQ(g[1].faces[2].style, std::string("Oblique"));
  CHECK_EQ(g[2].name, std::string("Fira Code"));
  CHECK(g[2].user);
  CHECK(g[2].monospace);
  CHECK_EQ(g[2].faces.size(), (size_t)1); // two formats of one face

  CHECK(is_font_file("x.TTF"));
  CHECK(is_font_file("a.pcf.gz"));
  CHECK(is_font_file("b.woff2"));
  CHECK(!is_font_file("readme.txt"));
  CHECK(is_user_font("/home/u/.local/share/fonts/a.ttf", home));
  CHECK(!is_user_font("/usr/share/fonts/a.ttf", home));
  CHECK(!is_user_font("/home/u/.local/share/fontsx/a.ttf", home));

  char tmpl[] = "/tmp/fld-fonts-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  CHECK_EQ(install_path(dir, "/x/Font.ttf"), dir + "/Font.ttf");
  fld::write_file(dir + "/Font.ttf", "x");
  CHECK_EQ(install_path(dir, "/y/Font.ttf"), dir + "/Font-2.ttf");
  std::string out;
  fld::run_capture({"rm", "-r", dir}, out);
  return test_result("fonts");
}
