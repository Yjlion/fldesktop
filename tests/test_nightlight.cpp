// fl-nightlight: colour temperature, schedules, sun times, zone table
#include <cstdio>
#include <cstdlib>
#include <string>

#include "nightlight_util.h"
#include "test.h"

using namespace nightlight;

static bool near(int a, int b, int tol) { return std::abs(a - b) <= tol; }

int main() {
  // 6500 K is neutral; warmer temperatures lose blue first
  Rgb n = temperature_rgb(6500);
  CHECK(std::fabs(n.r - 1) < 1e-9 && std::fabs(n.g - 1) < 1e-9 && std::fabs(n.b - 1) < 1e-9);
  Rgb w = temperature_rgb(3500);
  CHECK(w.r > 0.99);
  CHECK(w.g < 1 && w.b < w.g);
  auto ramp = gamma_ramp(256, w);
  CHECK_EQ(ramp.size(), (size_t)768);
  CHECK_EQ(ramp[0], (uint16_t)0);
  CHECK_EQ(ramp[255], (uint16_t)65535);  // red at full
  CHECK(ramp[767] < ramp[511]);           // blue below green

  // sun times (minutes after local midnight), within 10 minutes
  int rise, set;
  bool polar = false;
  CHECK(sun_times(172, 52.52, 13.40, 120, &rise, &set, &polar));  // Berlin, 21 June
  CHECK(near(rise, 4 * 60 + 43, 10));
  CHECK(near(set, 21 * 60 + 33, 10));
  CHECK(sun_times(355, 40.71, -74.0, -300, &rise, &set, &polar)); // New York, 21 December
  CHECK(near(rise, 7 * 60 + 17, 10));
  CHECK(near(set, 16 * 60 + 32, 10));
  CHECK(!sun_times(172, 69.65, 18.96, 120, &rise, &set, &polar));  // Tromsø: midnight sun
  CHECK(polar);
  CHECK(!sun_times(355, 69.65, 18.96, 60, &rise, &set, &polar));   // polar night
  CHECK(!polar);

  // zone1970.tab coordinates
  double lat, lon;
  CHECK(parse_iso6709("+5230+01322", &lat, &lon));
  CHECK(std::fabs(lat - 52.5) < 1e-9 && std::fabs(lon - 13.3667) < 1e-3);
  CHECK(parse_iso6709("+404251-0740023", &lat, &lon));
  CHECK(std::fabs(lat - 40.714) < 1e-3 && std::fabs(lon + 74.0064) < 1e-3);
  CHECK(parse_iso6709("-3352+15113", &lat, &lon));
  CHECK(lat < 0 && lon > 0);
  CHECK(!parse_iso6709("5230+01322", &lat, &lon));
  CHECK(!parse_iso6709("+52x0+01322", &lat, &lon));
  char tmpl[] = "/tmp/fl-nl-XXXXXX";
  int fd = mkstemp(tmpl);
  FILE *f = fdopen(fd, "w");
  fputs("# comment\nDE,DK\t+5230+01322\tEurope/Berlin\tmost of Germany\nUS\t+404251-0740023\tAmerica/New_York\n", f);
  fclose(f);
  CHECK(zone_coordinates(tmpl, "America/New_York", &lat, &lon));
  CHECK(std::fabs(lat - 40.714) < 1e-3);
  CHECK(!zone_coordinates(tmpl, "Europe/Paris", &lat, &lon));
  remove(tmpl);

  // schedules
  CHECK_EQ(parse_hhmm("20:30"), 20 * 60 + 30);
  CHECK_EQ(parse_hhmm("7:05"), 7 * 60 + 5);
  CHECK_EQ(parse_hhmm("24:00"), -1);
  CHECK_EQ(parse_hhmm("noon"), -1);
  int s = 20 * 60, e = 7 * 60;
  CHECK(night_amount(12 * 60, s, e, 30) == 0);       // midday
  CHECK(night_amount(23 * 60, s, e, 30) == 1);       // night
  CHECK(night_amount(3 * 60, s, e, 30) == 1);        // after midnight
  CHECK(std::fabs(night_amount(20 * 60 + 15, s, e, 30) - 0.5) < 1e-9); // fading in
  CHECK(std::fabs(night_amount(6 * 60 + 45, s, e, 30) - 0.5) < 1e-9);  // fading out
  CHECK(night_amount(7 * 60, s, e, 30) == 0);
  CHECK(night_amount(10, 0, 0, 30) == 0);            // empty night
  CHECK(night_amount(23 * 60, 22 * 60, 23 * 60 + 30, 0) == 1); // same-day night, no fade
  CHECK_EQ(blend_temperature(4000, 0), 6500);
  CHECK_EQ(blend_temperature(4000, 1), 4000);
  CHECK_EQ(blend_temperature(4000, 0.5), 5250);
  return test_result("nightlight");
}
