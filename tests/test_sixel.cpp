// fl-term: the Sixel decoder (apps/fl-term/sixel.cpp)
#include <string>

#include "sixel.h"
#include "test.h"

static bool px(const sixel::Image &im, int x, int y, int r, int g, int b, int a = 255) {
  const uint8_t *p = &im.rgba[((size_t)y * im.w + x) * 4];
  return p[0] == r && p[1] == g && p[2] == b && p[3] == a;
}

int main() {
  sixel::Image im;
  // a red 3x6 block: colour 1 = 100% red, three full sixels
  CHECK(sixel::decode("0;1;0", "#1;2;100;0;0#1~~~", im));
  CHECK_EQ(im.w, 3);
  CHECK_EQ(im.h, 6);
  CHECK(px(im, 0, 0, 255, 0, 0));
  CHECK(px(im, 2, 5, 255, 0, 0));

  // repeat, a second band, and bits: '@' is only the top pixel (bit 0)
  CHECK(sixel::decode("", "#2;2;0;100;0!4@-#3;2;0;0;100A", im));
  CHECK_EQ(im.w, 4);
  CHECK_EQ(im.h, 8);                // the second band's second row (bit 1)
  CHECK(px(im, 3, 0, 0, 255, 0));   // the repeat drew 4
  CHECK(px(im, 0, 1, 0, 0, 0, 0));  // not set: transparent
  CHECK(px(im, 0, 7, 0, 0, 255));   // 'A' = bit 1 of the band at y 6

  // $ goes back to the start of the band; later colours draw over
  CHECK(sixel::decode("", "#1;2;100;0;0~~$#2;2;0;100;0~", im));
  CHECK(px(im, 0, 0, 0, 255, 0));
  CHECK(px(im, 1, 0, 255, 0, 0));

  // raster attributes give the size even where nothing is drawn
  CHECK(sixel::decode("", "\"1;1;10;12#1~", im));
  CHECK_EQ(im.w, 10);
  CHECK_EQ(im.h, 12);

  // HLS: hue 120 is red in sixel's colour wheel
  CHECK(sixel::decode("", "#5;1;120;50;100#5~", im));
  CHECK(px(im, 0, 0, 255, 0, 0));
  // the default palette: register 2 is the VT340's red (80%)
  CHECK(sixel::decode("", "#2~", im));
  CHECK(px(im, 0, 0, 204, 33, 33));

  // limits: a huge repeat and far bands are cut at the maximum size
  CHECK(sixel::decode("", "#1!999999999~", im, 100, 100));
  CHECK_EQ(im.w, 100);
  std::string far;
  for (int i = 0; i < 100; i++) far += '-';
  CHECK(sixel::decode("", "#1~" + far + "~", im, 50, 50));
  CHECK_EQ(im.h, 6);  // the band past the limit drew nothing
  CHECK(sixel::decode("", "\"1;1;99999;99999#1~", im, 64, 64));
  CHECK_EQ(im.w, 64);
  CHECK_EQ(im.h, 64);

  // nothing drawn, or junk
  CHECK(!sixel::decode("", "", im));
  CHECK(!sixel::decode("", "#1;2;300;-5;9999$$--", im));
  CHECK(!sixel::decode("", "~", im, 0, 0));
  return test_result("sixel");
}
