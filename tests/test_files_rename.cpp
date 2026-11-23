// fl-files: batch rename
#include "rename_util.h"
#include "test.h"

using namespace frename;
using V = std::vector<std::string>;

int main() {
  CHECK_EQ(split_ext("photo.JPG").second, std::string(".JPG"));
  CHECK_EQ(split_ext(".bashrc").second, std::string(""));
  CHECK_EQ(split_ext("README").first, std::string("README"));
  CHECK_EQ(split_ext("a.tar.gz").first, std::string("a.tar"));

  Rule r;
  CHECK(apply({"a.txt", "b"}, r) == V({"a.txt", "b"}));
  r.pattern = "Holiday {n:3}{ext}";
  CHECK(apply({"x.jpg", "y.png"}, r) == V({"Holiday 001.jpg", "Holiday 002.png"}));
  r.start = 9;
  r.step = 2;
  CHECK(apply({"x.jpg", "y.jpg"}, r) == V({"Holiday 009.jpg", "Holiday 011.jpg"}));
  r.pattern = "{nope}-{name}";
  r.start = 1;
  r.step = 1;
  CHECK(apply({"a.b"}, r) == V({"{nope}-a"}));

  Rule f;
  f.find = "IMG_";
  f.replace = "";
  CHECK(apply({"IMG_1.jpg", "img_2.jpg"}, f) == V({"1.jpg", "img_2.jpg"}));
  f.ignore_case = true;
  CHECK(apply({"img_2.jpg"}, f) == V({"2.jpg"}));
  Rule re;
  re.regex = true;
  re.find = "(\\d+)-(\\d+)";
  re.replace = "$2-$1";
  CHECK(apply({"10-20.txt"}, re) == V({"20-10.txt"}));
  re.find = "(";
  std::string err;
  CHECK(apply({"a"}, re, &err).empty());
  CHECK(!err.empty());

  Rule c;
  c.letter_case = Case::Lower;
  CHECK(apply({"My Photo.JPG"}, c) == V({"my photo.JPG"}));
  c.letter_case = Case::Title;
  CHECK(apply({"the cat's hat.txt"}, c) == V({"The Cat's Hat.txt"}));
  c.letter_case = Case::Upper;
  CHECK(apply({"ünï.txt"}, c) == V({"üNï.txt"}));

  auto p = problems({"a", "b", "c", "d"}, {"x", "x", "", "a/b"});
  CHECK_EQ(p[0], std::string("used twice"));
  CHECK_EQ(p[2], std::string("empty name"));
  CHECK_EQ(p[3], std::string("contains /"));
  CHECK(problems({"a"}, {".."})[0] != "");

  // a swap goes through a temporary name; unchanged names are left out
  auto pl = plan({"a", "b", "c"}, {"b", "a", "c"}, ".tmp-");
  CHECK_EQ(pl.size(), (size_t)4);
  CHECK(pl[0] == std::make_pair(std::string("a"), std::string(".tmp-0")));
  CHECK(pl[1] == std::make_pair(std::string("b"), std::string(".tmp-1")));
  CHECK(pl[2] == std::make_pair(std::string(".tmp-0"), std::string("b")));
  CHECK(pl[3] == std::make_pair(std::string(".tmp-1"), std::string("a")));
  // a chain: 1 -> 2, 2 -> 3
  pl = plan({"1", "2"}, {"2", "3"}, "t");
  CHECK_EQ(pl.size(), (size_t)3);
  CHECK(pl[0] == std::make_pair(std::string("1"), std::string("t0")));
  CHECK(pl[1] == std::make_pair(std::string("2"), std::string("3")));
  return test_result("files-rename");
}
