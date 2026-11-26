// fl-wm: the tiling layout
extern "C" {
#include "tile.h"
}
#include "test.h"

int main() {
  flwm_tile_box out[4];
  tile_layout({0, 0, 1000, 600}, 1, 0.5, 10, out);
  CHECK_EQ(out[0].x, 10);
  CHECK_EQ(out[0].w, 980);
  CHECK_EQ(out[0].h, 580);
  tile_layout({0, 0, 1000, 600}, 3, 0.6, 10, out);
  CHECK_EQ(out[0].x, 10);
  CHECK_EQ(out[0].w, 582);           // (980 - 10) * 0.6
  CHECK_EQ(out[1].x, 10 + 582 + 10);
  CHECK_EQ(out[1].x + out[1].w, 990); // to the right edge, minus the gap
  CHECK_EQ(out[1].y, 10);
  CHECK_EQ(out[2].y + out[2].h, 590); // to the bottom
  CHECK_EQ(out[2].y, out[1].y + out[1].h + 10);
  tile_layout({100, 50, 800, 400}, 2, 5.0, 0, out); // the ratio is limited
  CHECK_EQ(out[0].w, 640);
  CHECK_EQ(out[0].x, 100);
  CHECK_EQ(out[1].y, 50);
  return test_result("tile");
}
