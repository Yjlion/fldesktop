// fl-wm: panel popups go to the button that opened them
#include "popup_place.h"
#include "test.h"

int main() {
  int a, b;
  CHECK(flwm_popup_title("fldesktop.panel-popup", &a, &b));
  CHECK(a == -1 && b == -1);
  CHECK(flwm_popup_title("fldesktop.panel-popup@1290-1320", &a, &b));
  CHECK_EQ(a, 1290);
  CHECK_EQ(b, 1320);
  CHECK(flwm_popup_title("fldesktop.panel-popup@12x", &a, &b) && a == -1); // no range: the pointer
  CHECK(flwm_popup_title("fldesktop.panel-popup@30-20", &a, &b) && a == -1);
  CHECK(!flwm_popup_title("fldesktop.panel-popups", &a, &b));
  CHECK(!flwm_popup_title("fldesktop.panel", &a, &b));
  CHECK(!flwm_popup_title(nullptr, &a, &b));
  // centred on the button, kept inside the screen
  CHECK_EQ(flwm_popup_x(500, 200, 0, 1366), 400);
  CHECK_EQ(flwm_popup_x(1350, 260, 0, 1366), 1366 - 4 - 260);
  CHECK_EQ(flwm_popup_x(10, 260, 0, 1366), 4);
  CHECK_EQ(flwm_popup_x(10, 260, 1366, 1920), 1370); // a second output
  return test_result("popup-place");
}
