// fl-wm: what a finished touchpad swipe means
extern "C" {
#include "gesture_decide.h"
}
#include "test.h"

int main() {
  // natural: swiping left brings in the next workspace
  CHECK(flwm_swipe_decide(-300, 20, true, false) == FLWM_SWIPE_WORKSPACE_NEXT);
  CHECK(flwm_swipe_decide(300, -20, true, false) == FLWM_SWIPE_WORKSPACE_PREV);
  CHECK(flwm_swipe_decide(-300, 20, false, false) == FLWM_SWIPE_WORKSPACE_PREV);
  CHECK(flwm_swipe_decide(300, 0, false, false) == FLWM_SWIPE_WORKSPACE_NEXT);
  // up and down do not depend on the direction setting
  CHECK(flwm_swipe_decide(10, -250, true, false) == FLWM_SWIPE_UP);
  CHECK(flwm_swipe_decide(10, 250, false, false) == FLWM_SWIPE_DOWN);
  // too short, diagonal or cancelled: nothing
  CHECK(flwm_swipe_decide(-60, 0, true, false) == FLWM_SWIPE_NONE);
  CHECK(flwm_swipe_decide(-200, 180, true, false) == FLWM_SWIPE_NONE);
  CHECK(flwm_swipe_decide(-300, 0, true, true) == FLWM_SWIPE_NONE);
  CHECK(flwm_swipe_decide(0, 0, true, false) == FLWM_SWIPE_NONE);
  return test_result("gesture");
}
