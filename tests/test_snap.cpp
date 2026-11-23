// fl-wm: snapping geometry, Super+arrow transitions and drag zones
extern "C" {
#include "snap.h"
}
#include "test.h"

static bool eq(flwm_rect r, int x, int y, int w, int h) { return r.x == x && r.y == y && r.width == w && r.height == h; }

int main() {
  flwm_rect a = {0, 30, 1280, 690}; // a panel above
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_LEFT, a), 0, 30, 640, 690));
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_RIGHT, a), 640, 30, 640, 690));
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_TOP_LEFT, a), 0, 30, 640, 345));
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_BOTTOM_RIGHT, a), 640, 375, 640, 345));
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_MAXIMIZED, a), 0, 30, 1280, 690));
  flwm_rect odd = {100, 0, 1001, 701}; // odd sizes: the right/bottom part takes the extra pixel's place
  flwm_rect br = flwm_snap_rect(FLWM_SNAP_BOTTOM_RIGHT, odd);
  CHECK(br.x + br.width == 1101 && br.y + br.height == 701);

  // Super+arrows
  CHECK(flwm_snap_next(FLWM_SNAP_NONE, FLWM_SNAP_DIR_LEFT) == FLWM_SNAP_LEFT);
  CHECK(flwm_snap_next(FLWM_SNAP_LEFT, FLWM_SNAP_DIR_RIGHT) == FLWM_SNAP_NONE);
  CHECK(flwm_snap_next(FLWM_SNAP_LEFT, FLWM_SNAP_DIR_UP) == FLWM_SNAP_TOP_LEFT);
  CHECK(flwm_snap_next(FLWM_SNAP_TOP_LEFT, FLWM_SNAP_DIR_RIGHT) == FLWM_SNAP_TOP_RIGHT);
  CHECK(flwm_snap_next(FLWM_SNAP_TOP_RIGHT, FLWM_SNAP_DIR_DOWN) == FLWM_SNAP_RIGHT);
  CHECK(flwm_snap_next(FLWM_SNAP_RIGHT, FLWM_SNAP_DIR_DOWN) == FLWM_SNAP_BOTTOM_RIGHT);
  CHECK(flwm_snap_next(FLWM_SNAP_BOTTOM_RIGHT, FLWM_SNAP_DIR_DOWN) == FLWM_SNAP_BOTTOM_RIGHT);
  CHECK(flwm_snap_next(FLWM_SNAP_NONE, FLWM_SNAP_DIR_UP) == FLWM_SNAP_MAXIMIZED);
  CHECK(flwm_snap_next(FLWM_SNAP_MAXIMIZED, FLWM_SNAP_DIR_DOWN) == FLWM_SNAP_NONE);
  CHECK(flwm_snap_next(FLWM_SNAP_NONE, FLWM_SNAP_DIR_DOWN) == FLWM_SNAP_MINIMIZED);
  CHECK(flwm_snap_next(FLWM_SNAP_MAXIMIZED, FLWM_SNAP_DIR_LEFT) == FLWM_SNAP_LEFT);

  // drag zones on a 1280x720 output at x 1280 (the second screen)
  flwm_rect o = {1280, 0, 1280, 720};
  CHECK(flwm_snap_zone(1281, 360, o, 8, 64) == FLWM_SNAP_LEFT);
  CHECK(flwm_snap_zone(2559, 360, o, 8, 64) == FLWM_SNAP_RIGHT);
  CHECK(flwm_snap_zone(1900, 2, o, 8, 64) == FLWM_SNAP_MAXIMIZED);
  CHECK(flwm_snap_zone(1282, 20, o, 8, 64) == FLWM_SNAP_TOP_LEFT);
  CHECK(flwm_snap_zone(1300, 3, o, 8, 64) == FLWM_SNAP_TOP_LEFT);
  CHECK(flwm_snap_zone(2558, 700, o, 8, 64) == FLWM_SNAP_BOTTOM_RIGHT);
  CHECK(flwm_snap_zone(1900, 360, o, 8, 64) == FLWM_SNAP_NONE);
  CHECK(flwm_snap_zone(1900, 718, o, 8, 64) == FLWM_SNAP_NONE); // the bottom edge (panel) is no zone
  // thirds
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_LEFT_THIRD, a), 0, 30, 426, 690));
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_CENTER_THIRD, a), 426, 30, 428, 690));
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_RIGHT_THIRD, a), 854, 30, 426, 690));
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_LEFT_TWO_THIRDS, a), 0, 30, 854, 690));
  CHECK(eq(flwm_snap_rect(FLWM_SNAP_RIGHT_TWO_THIRDS, a), 426, 30, 854, 690));
  // Super+Left again on the left half: two thirds, one third, half
  CHECK(flwm_snap_next(FLWM_SNAP_LEFT, FLWM_SNAP_DIR_LEFT) == FLWM_SNAP_LEFT_TWO_THIRDS);
  CHECK(flwm_snap_next(FLWM_SNAP_LEFT_TWO_THIRDS, FLWM_SNAP_DIR_LEFT) == FLWM_SNAP_LEFT_THIRD);
  CHECK(flwm_snap_next(FLWM_SNAP_LEFT_THIRD, FLWM_SNAP_DIR_LEFT) == FLWM_SNAP_LEFT);
  CHECK(flwm_snap_next(FLWM_SNAP_LEFT_THIRD, FLWM_SNAP_DIR_RIGHT) == FLWM_SNAP_NONE);
  CHECK(flwm_snap_next(FLWM_SNAP_RIGHT, FLWM_SNAP_DIR_RIGHT) == FLWM_SNAP_RIGHT_TWO_THIRDS);
  CHECK(flwm_snap_next(FLWM_SNAP_CENTER_THIRD, FLWM_SNAP_DIR_LEFT) == FLWM_SNAP_LEFT_THIRD);
  CHECK(flwm_snap_next_third(FLWM_SNAP_NONE, FLWM_SNAP_DIR_RIGHT) == FLWM_SNAP_RIGHT_THIRD);
  CHECK(flwm_snap_next_third(FLWM_SNAP_LEFT_THIRD, FLWM_SNAP_DIR_RIGHT) == FLWM_SNAP_CENTER_THIRD);
  CHECK(flwm_snap_next_third(FLWM_SNAP_CENTER_THIRD, FLWM_SNAP_DIR_RIGHT) == FLWM_SNAP_RIGHT_THIRD);
  CHECK(flwm_snap_next_third(FLWM_SNAP_RIGHT_THIRD, FLWM_SNAP_DIR_LEFT) == FLWM_SNAP_CENTER_THIRD);
  CHECK(flwm_snap_next_third(FLWM_SNAP_MAXIMIZED, FLWM_SNAP_DIR_UP) == FLWM_SNAP_MAXIMIZED);
  // what snap assist offers
  CHECK(flwm_snap_complement(FLWM_SNAP_LEFT) == FLWM_SNAP_RIGHT);
  CHECK(flwm_snap_complement(FLWM_SNAP_LEFT_THIRD) == FLWM_SNAP_RIGHT_TWO_THIRDS);
  CHECK(flwm_snap_complement(FLWM_SNAP_RIGHT_TWO_THIRDS) == FLWM_SNAP_LEFT_THIRD);
  CHECK(flwm_snap_complement(FLWM_SNAP_TOP_LEFT) == FLWM_SNAP_NONE);
  CHECK(flwm_snap_complement(FLWM_SNAP_MAXIMIZED) == FLWM_SNAP_NONE);
  return test_result("snap");
}
