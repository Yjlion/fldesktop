// fl-clock: durations, alarms, zones
#include <unistd.h>

#include <cstdlib>

#include "clock_util.h"
#include "test.h"

using namespace clk;

int main() {
  CHECK_EQ(format_duration(0), std::string("00:00"));
  CHECK_EQ(format_duration(125), std::string("02:05"));
  CHECK_EQ(format_duration(3725), std::string("1:02:05"));
  CHECK_EQ(format_duration(65.37, true), std::string("01:05.3"));
  CHECK_NEAR(parse_duration("5"), 300.0);
  CHECK_NEAR(parse_duration("1:30"), 90.0);
  CHECK_NEAR(parse_duration("1:02:03"), 3723.0);
  CHECK_NEAR(parse_duration("90s"), 90.0);
  CHECK_NEAR(parse_duration("1h30m"), 5400.0);
  CHECK_NEAR(parse_duration("2m 30s"), 150.0);
  CHECK(parse_duration("") < 0);
  CHECK(parse_duration("abc") < 0);
  CHECK(parse_duration("1:x") < 0);

  int h, m;
  CHECK(parse_hhmm("07:30", h, m) && h == 7 && m == 30);
  CHECK(!parse_hhmm("24:00", h, m));
  CHECK(!parse_hhmm("7.30", h, m));

  setenv("TZ", "UTC", 1);
  tzset();
  // 2026-11-23 is a Monday; 08:00 UTC
  struct tm tm {};
  tm.tm_year = 126;
  tm.tm_mon = 10;
  tm.tm_mday = 23;
  tm.tm_hour = 8;
  time_t mon8 = timegm(&tm);
  Alarm a;
  a.hour = 7;
  a.minute = 0;
  a.days = 0x3e; // weekdays
  CHECK_EQ(next_ring(a, mon8), mon8 + 23 * 3600); // Tuesday 07:00
  a.days = 0;                                     // once: tomorrow 07:00
  CHECK_EQ(next_ring(a, mon8), mon8 + 23 * 3600);
  a.hour = 9;
  CHECK_EQ(next_ring(a, mon8), mon8 + 3600); // today 09:00
  a.days = 1 << 6;                           // Saturdays
  CHECK_EQ(next_ring(a, mon8), mon8 + 5 * 86400 + 3600);
  a.snoozed_until = mon8 + 300;
  CHECK_EQ(next_ring(a, mon8), mon8 + 300);
  a.enabled = false;
  CHECK_EQ(next_ring(a, mon8), (time_t)0);

  CHECK_EQ(days_text(0), std::string("Once"));
  CHECK_EQ(days_text(0x7f), std::string("Every day"));
  CHECK_EQ(days_text(0x3e), std::string("Weekdays"));
  CHECK_EQ(days_text((1 << 0) | (1 << 3)), std::string("Wed Sun"));

  std::vector<Alarm> list(2);
  list[0].id = "b";
  list[0].hour = 9;
  list[0].label = "Standup";
  list[0].days = 0x3e;
  list[1].id = "a";
  list[1].hour = 6;
  list[1].minute = 45;
  list[1].enabled = false;
  auto back = alarms_from_ini(alarms_to_ini(list));
  CHECK_EQ(back.size(), (size_t)2);
  CHECK_EQ(back[0].id, std::string("a")); // sorted by time
  CHECK(!back[0].enabled);
  CHECK_EQ(back[0].minute, 45);
  CHECK_EQ(back[1].label, std::string("Standup"));
  CHECK_EQ(back[1].days, 0x3e);

  auto zones = parse_zone_tab("# comment\nGB\t+513030-0000731\tEurope/London\nUS\t+404251-0740023\tAmerica/New_York\t"
                              "Eastern (most areas)\nJP\t+353916+1394441\tAsia/Tokyo\n");
  CHECK_EQ(zones.size(), (size_t)3);
  CHECK_EQ(zones[0], std::string("America/New_York"));
  CHECK_EQ(city_of("America/New_York"), std::string("New York"));
  CHECK_EQ(city_of("America/Argentina/Buenos_Aires"), std::string("Buenos Aires"));
  CHECK_EQ(city_of("UTC"), std::string("UTC"));
  // zone offsets need tzdata
  if (access("/usr/share/zoneinfo/Asia/Tokyo", R_OK) == 0) {
    CHECK_EQ(offset_text("Asia/Tokyo", mon8), std::string("+9 h"));
    CHECK_EQ(offset_text("Asia/Kolkata", mon8), std::string("+5:30"));
    CHECK_EQ(offset_text("UTC", mon8), std::string("same time"));
    CHECK_EQ(time_in("Asia/Tokyo", mon8).tm_hour, 17);
    CHECK_EQ(time_in("", mon8).tm_hour, 8); // TZ is back
  }
  return test_result("clock");
}
