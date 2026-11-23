// fl-clock: alarms, durations and time zones (unit tested)
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace clk {

// 3725.4 -> "1:02:05", with tenths "1:02:05.4"; under an hour "02:05"
std::string format_duration(double seconds, bool tenths = false);
// "10", "1:30", "1:02:03", "90s", "5m", "1h30m" -> seconds; -1 if not a duration
double parse_duration(const std::string &text);

struct Alarm {
  std::string id;     // a stable key in clock.ini
  int hour = 7, minute = 0;
  int days = 0;       // bit 0 Sunday .. bit 6 Saturday; 0 = once
  std::string label;
  bool enabled = true;
  time_t snoozed_until = 0;
  time_t last_rang = 0; // so it rings once per time
};
// "07:30" -> hour, minute
bool parse_hhmm(const std::string &s, int &h, int &m);
// When the alarm rings next after `now` (local time, DST aware); 0 if disabled.
time_t next_ring(const Alarm &a, time_t now);
// "Mon Tue Wed", "Weekdays", "Every day", "Once"
std::string days_text(int days);

// Alarms in clock.ini: [alarm:<id>] Time=07:30 Days=62 Label= Enabled=true
std::string alarms_to_ini(const std::vector<Alarm> &alarms);
std::vector<Alarm> alarms_from_ini(const std::string &text);

// zone1970.tab / zone.tab: the zone names; "America/New_York" -> "New York"
std::vector<std::string> parse_zone_tab(const std::string &text);
std::string city_of(const std::string &zone);
// the local time in a zone ("" for the local zone) at `t`
struct tm time_in(const std::string &zone, time_t t);
// the hours between a zone and the local one ("+5", "-3:30", "same time")
std::string offset_text(const std::string &zone, time_t t);

} // namespace clk
