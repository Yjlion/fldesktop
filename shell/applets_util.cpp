#include "applets_util.h"

#include <cctype>

#include <cstring>

#include <fstream>

#include <set>

#include <unistd.h>

#include <dirent.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "fld/json.h"
#include "fld/util.h"

namespace applets {

std::vector<Place> parse_places(const std::string &json) {
  std::vector<Place> out;
  fld::Json j;
  if (!fld::Json::parse(json, j)) return out;
  for (const fld::Json &r : j["results"].items()) {
    Place p;
    p.name = r["name"].as_string();
    p.region = r["admin1"].as_string();
    p.country = r["country"].as_string();
    p.lat = r["latitude"].as_number();
    p.lon = r["longitude"].as_number();
    if (!p.name.empty()) out.push_back(p);
  }
  return out;
}

std::string search_url(const std::string &name) {
  std::string q;
  char b[4];
  for (unsigned char c : name) {
    if (isalnum(c) || c == '-' || c == '.' || c == '_') q += (char)c;
    else {
      snprintf(b, sizeof b, "%%%02X", c);
      q += b;
    }
  }
  return "https://geocoding-api.open-meteo.com/v1/search?count=6&format=json&name=" + q;
}

std::string forecast_url(double lat, double lon, bool fahrenheit) {
  char b[512];
  snprintf(b, sizeof b,
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=4&timezone=auto%s",
           lat, lon, fahrenheit ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
  return b;
}

Weather parse_forecast(const std::string &json) {
  Weather w;
  fld::Json j;
  if (!fld::Json::parse(json, j) || !j["current"].is_object()) return w;
  const fld::Json &c = j["current"];
  w.temp = c["temperature_2m"].as_number();
  w.feels = c["apparent_temperature"].is_null() ? w.temp : c["apparent_temperature"].as_number();
  w.code = (int)c["weather_code"].as_number();
  w.wind = c["wind_speed_10m"].as_number();
  w.humidity = c["relative_humidity_2m"].is_null() ? -1 : (int)c["relative_humidity_2m"].as_number();
  w.day = c["is_day"].is_null() || c["is_day"].as_number() != 0;
  const fld::Json &d = j["daily"];
  const auto &dates = d["time"].items();
  for (size_t i = 0; i < dates.size(); i++) {
    Day day;
    day.date = dates[i].as_string();
    if (i < d["weather_code"].items().size()) day.code = (int)d["weather_code"].items()[i].as_number();
    if (i < d["temperature_2m_min"].items().size()) day.tmin = d["temperature_2m_min"].items()[i].as_number();
    if (i < d["temperature_2m_max"].items().size()) day.tmax = d["temperature_2m_max"].items()[i].as_number();
    w.days.push_back(day);
  }
  w.ok = true;
  return w;
}

std::string weather_text(int c) {
  if (c == 0) return "Clear";
  if (c == 1) return "Mainly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Overcast";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Rain showers";
  if (c == 85 || c == 86) return "Snow showers";
  if (c >= 95) return "Thunderstorm";
  return "Unknown";
}

std::string weather_icon(int c, bool day) {
  if (c == 0 || c == 1) return day ? "weather-clear" : "weather-clear-night";
  if (c == 2) return day ? "weather-few-clouds" : "weather-few-clouds-night";
  if (c == 3) return "weather-overcast";
  if (c == 45 || c == 48) return "weather-fog";
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return "weather-showers";
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return "weather-snow";
  if (c >= 95) return "weather-storm";
  return "weather-severe-alert";
}

std::string degrees(double t) {
  long r = std::lround(t);
  return std::to_string(r == 0 ? 0 : r) + "\xc2\xb0";
}

bool parse_proc_stat(const std::string &text, CpuTimes &t) {
  std::istringstream in(text);
  std::string label;
  if (!(in >> label) || label != "cpu") return false;
  // user nice system idle iowait irq softirq steal
  unsigned long long v[8] = {0};
  for (int i = 0; i < 8 && (in >> v[i]); i++) {}
  unsigned long long idle = v[3] + v[4];
  t.total = 0;
  for (unsigned long long x : v) t.total += x;
  t.busy = t.total - idle;
  return t.total > 0;
}

double cpu_usage(const CpuTimes &a, const CpuTimes &b) {
  if (b.total <= a.total) return 0;
  double u = (double)(b.busy - a.busy) / (double)(b.total - a.total);
  return u < 0 ? 0 : u > 1 ? 1 : u;
}

double memory_usage(const std::string &meminfo, unsigned long long *total_kb) {
  unsigned long long total = 0, avail = 0;
  for (const std::string &line : fld::split(meminfo, '\n')) {
    if (fld::starts_with(line, "MemTotal:")) total = strtoull(line.c_str() + 9, nullptr, 10);
    else if (fld::starts_with(line, "MemAvailable:")) avail = strtoull(line.c_str() + 13, nullptr, 10);
  }
  if (total_kb) *total_kb = total;
  if (!total) return 0;
  return (double)(total - std::min(avail, total)) / (double)total;
}

// ---- privacy ----

std::vector<std::string> camera_users(const std::string &proc) {
  std::set<std::string> names;
  DIR *d = opendir(proc.c_str());
  if (!d) return {};
  while (struct dirent *e = readdir(d)) {
    if (!std::isdigit((unsigned char)e->d_name[0])) continue;
    std::string pdir = proc + "/" + e->d_name;
    DIR *fd = opendir((pdir + "/fd").c_str());
    if (!fd) continue; // another user's process
    bool cam = false;
    while (struct dirent *f = readdir(fd)) {
      if (f->d_name[0] == '.') continue;
      char target[256];
      ssize_t n = readlink((pdir + "/fd/" + f->d_name).c_str(), target, sizeof target - 1);
      if (n <= 0) continue;
      target[n] = 0;
      if (strncmp(target, "/dev/video", 10) == 0) {
        cam = true;
        break;
      }
    }
    closedir(fd);
    if (!cam) continue;
    std::string comm;
    std::ifstream c(pdir + "/comm");
    std::getline(c, comm);
    if (!comm.empty()) names.insert(comm);
  }
  closedir(d);
  return {names.begin(), names.end()};
}

std::string name_list(const std::vector<std::string> &n, size_t max) {
  if (n.empty()) return "";
  if (n.size() == 1) return n[0];
  if (n.size() <= max) {
    std::string s;
    for (size_t i = 0; i + 1 < n.size(); i++) s += (i ? ", " : "") + n[i];
    return s + " and " + n.back();
  }
  std::string s;
  for (size_t i = 0; i < max; i++) s += (i ? ", " : "") + n[i];
  return s + " and " + std::to_string(n.size() - max) + " more";
}

} // namespace applets
