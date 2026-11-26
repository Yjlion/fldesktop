// fl-wm animations: the easing (compositor/anim.h)
#include <cmath>

extern "C" {
#include "anim.h"
}
#include "test.h"

int main() {
  CHECK(anim_ease(0, 160) == 0.0);
  CHECK(anim_ease(-5, 160) == 0.0);
  CHECK(anim_ease(160, 160) == 1.0);
  CHECK(anim_ease(1000, 160) == 1.0);
  CHECK(anim_ease(10, 0) == 1.0);  // no duration: at the end at once
  // eased out: more than half way at half the time, and never going back
  CHECK(anim_ease(80, 160) > 0.8);
  double last = 0;
  bool monotonic = true;
  for (int t = 0; t <= 160; t += 4) {
    double e = anim_ease(t, 160);
    if (e < last || e < 0 || e > 1) monotonic = false;
    last = e;
  }
  CHECK(monotonic);
  CHECK(std::fabs(anim_ease(40, 160) - (1 - std::pow(0.75, 3))) < 1e-9);
  return test_result("wm-anim");
}
