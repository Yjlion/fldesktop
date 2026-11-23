// fl-panel's weather and system monitor applets: the parts without FLTK
// (unit tested)
#pragma once
#include <string>
#include <vector>

namespace applets {

// ---- weather (Open-Meteo) ----
struct Place {
  std::string name, region, country;
  double lat = 0, lon = 0;
};
// https://geocoding-api.open-meteo.com/v1/search?name=... results
std::vector<Place> parse_places(const std::string &json);

struct Day {
  std::string date; // YYYY-MM-DD
  int code = 0;
  double tmin = 0, tmax = 0;
};
struct Weather {
  bool ok = false;
  double temp = 0, feels = 0, wind = 0;
  int code = 0, humidity = -1;
  bool day = true;
  std::vector<Day> days;
};
// the forecast URL for a place (current and 4 days)
std::string forecast_url(double lat, double lon, bool fahrenheit);
std::string search_url(const std::string &name);
Weather parse_forecast(const std::string &json);
// WMO weather codes: a description and a theme icon (weather-clear, ...)
std::string weather_text(int code);
std::string weather_icon(int code, bool day);
// "12°", "-3°"
std::string degrees(double t);

// ---- system monitor ----
struct CpuTimes {
  unsigned long long busy = 0, total = 0;
};
// the "cpu " line of /proc/stat
bool parse_proc_stat(const std::string &text, CpuTimes &t);
// 0..1 between two readings
double cpu_usage(const CpuTimes &a, const CpuTimes &b);
// used memory (MemTotal - MemAvailable) / MemTotal from /proc/meminfo
double memory_usage(const std::string &meminfo, unsigned long long *total_kb = nullptr);

} // namespace applets
