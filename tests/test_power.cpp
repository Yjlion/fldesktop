// UPower helpers, and (with FLD_UPOWER_MOCK=1, run by upower-mock.sh against
// python-dbusmock's upower template) the client against a mock UPower.
#include <FL/Fl.H>

#include <cstdlib>
#include <functional>

#include "test.h"
#include "upclient.h"

static bool wait_for(std::function<bool()> cond, double secs = 5) {
  for (double t = 0; t < secs; t += 0.02) {
    if (cond()) return true;
    Fl::wait(0.02);
  }
  return cond();
}

int main() {
  using namespace up;
  CHECK_EQ(icon_for(95, DISCHARGING, true), std::string("battery-full"));
  CHECK_EQ(icon_for(60, CHARGING, false), std::string("battery-good-charging"));
  CHECK_EQ(icon_for(30, DISCHARGING, true), std::string("battery-low"));
  CHECK_EQ(icon_for(10, DISCHARGING, true), std::string("battery-caution"));
  CHECK_EQ(icon_for(2, DISCHARGING, true), std::string("battery-empty"));
  CHECK_EQ(icon_for(50, FULL, false), std::string("battery-full-charging"));
  CHECK_EQ(icon_for(-1, UNKNOWN, false), std::string("battery-missing"));
  CHECK_EQ(format_time(0), std::string(""));
  CHECK_EQ(format_time(35 * 60), std::string("35 min"));
  CHECK_EQ(format_time(2 * 3600 + 5 * 60 + 10), std::string("2 h 05 min"));
  State s;
  CHECK_EQ(summary(s), std::string("On AC power"));
  s.has_battery = true;
  s.display.percentage = 84.6;
  s.display.state = DISCHARGING;
  s.display.time_to_empty = 3600;
  CHECK_EQ(summary(s), std::string("85%, 1 h 00 min left"));
  s.display.state = FULL;
  CHECK_EQ(summary(s), std::string("85%, fully charged"));
  CHECK_EQ(profile_label("power-saver"), std::string("Power Saver"));
  CHECK_EQ(profile_label("custom"), std::string("custom"));
  CHECK_EQ(battery_level(50, true), LEVEL_OK);
  CHECK_EQ(battery_level(9, true), LEVEL_LOW);
  CHECK_EQ(battery_level(5, true), LEVEL_CRITICAL);
  CHECK_EQ(battery_level(2, true), LEVEL_ACTION);
  CHECK_EQ(battery_level(2, false), LEVEL_OK);
  CHECK_EQ(battery_level(15, true, 20, 10, 5), LEVEL_LOW);
  Device d;
  d.type = 2;
  CHECK_EQ(d.name(), std::string("Battery"));
  d.type = 5;
  d.vendor = "Logitech";
  d.model = "MX";
  CHECK_EQ(d.name(), std::string("Logitech MX"));

  if (getenv("FLD_UPOWER_MOCK")) {
    Fl::lock();
    Client &c = Client::get();
    c.start();
    CHECK(wait_for([&] { return c.state().running && c.state().has_battery; }));
    CHECK(c.state().on_battery);
    CHECK_NEAR(c.state().display.percentage, 30);
    CHECK_EQ(c.state().display.state, (int)DISCHARGING);
    bool bat = false;
    for (auto &x : c.state().devices) if (x.type == 2) bat = true;
    CHECK(bat);
  }
  return test_result("power");
}
