// fl-autorotate: orientations and outputs
#include "rotate_util.h"
#include "test.h"

int main() {
  CHECK_EQ(rotate::transform_for("normal"), 0);
  CHECK_EQ(rotate::transform_for("left-up"), 1);
  CHECK_EQ(rotate::transform_for("bottom-up"), 2);
  CHECK_EQ(rotate::transform_for("right-up"), 3);
  CHECK_EQ(rotate::transform_for("undefined"), -1);
  CHECK(rotate::is_internal("eDP-1"));
  CHECK(rotate::is_internal("LVDS-1"));
  CHECK(rotate::is_internal("DSI-1"));
  CHECK(!rotate::is_internal("HDMI-A-1"));
  CHECK(!rotate::is_internal("DP-2"));
  return test_result("rotate");
}
