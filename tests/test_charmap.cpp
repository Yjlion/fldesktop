#include "charmap_data.h"
#include "test.h"

int main() {
  auto blocks = charmap::parse_blocks("# comment\n0000..007F; Basic Latin\n0080..00FF; Latin-1 Supplement\n\nbad line\n");
  CHECK_EQ(blocks.size(), 2u);
  CHECK_EQ(blocks[1].first, 0x80u);
  CHECK_EQ(blocks[1].name, std::string("Latin-1 Supplement"));

  charmap::NameDb db;
  CHECK(db.parse("0009;<control>;Cc;0;S;;;;;N;CHARACTER TABULATION;;;;\n"
                 "0041;LATIN CAPITAL LETTER A;Lu;0;L;;;;;N;;;;0061;\n"
                 "2192;RIGHTWARDS ARROW;Sm;0;ON;;;;;N;RIGHT ARROW;;;;\n"
                 "21D2;RIGHTWARDS DOUBLE ARROW;Sm;0;ON;;;;;N;RIGHT DOUBLE ARROW;;;;\n"
                 "4E00;<CJK Ideograph, First>;Lo;0;L;;;;;N;;;;;\n"
                 "9FFF;<CJK Ideograph, Last>;Lo;0;L;;;;;N;;;;;\n"
                 "E000;<Private Use, First>;Co;0;L;;;;;N;;;;;\n"
                 "F8FF;<Private Use, Last>;Co;0;L;;;;;N;;;;;\n"));
  CHECK_EQ(db.name(0x41), std::string("LATIN CAPITAL LETTER A"));
  CHECK_EQ(db.name(0x09), std::string("CHARACTER TABULATION"));
  CHECK_EQ(db.name(0x6F22), std::string("CJK UNIFIED IDEOGRAPH-6F22"));
  CHECK_EQ(db.category(0x6F22), std::string("Lo"));
  CHECK(db.printable(0x41));
  CHECK(!db.printable(0x09));
  CHECK(!db.printable(0xE123));
  CHECK(!db.printable(0x42)); // unassigned in this small db
  auto r = db.search("rightwards arrow");
  CHECK_EQ(r.size(), 2u);
  CHECK_EQ(r[0], 0x2192u);
  CHECK_EQ(db.search("double arrow").size(), 1u);
  CHECK_EQ(db.search("U+21D2")[0], 0x21D2u);
  CHECK_EQ(db.search("→")[0], 0x2192u);
  CHECK_EQ(db.search("rightwards arrow")[0], 0x2192u);
  CHECK_EQ(charmap::utf8(0x20AC), std::string("€"));
  CHECK_EQ(charmap::first_codepoint("€uro"), 0x20ACu);
  CHECK_EQ(charmap::first_codepoint("😀"), 0x1F600u);

  // the real database, when installed
  std::string ud = charmap::find_unicode_data();
  if (!ud.empty()) {
    charmap::NameDb full;
    CHECK(full.load(ud));
    CHECK_EQ(full.name(0x1F600), std::string("GRINNING FACE"));
    CHECK(!full.search("snowman").empty());
  }
  return test_result("charmap");
}
