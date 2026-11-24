#include "clock_util.h"

#include <map>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fld/ini.h"
#include "fld/util.h"

namespace clk {

std::string format_duration(double s, bool tenths) {
  if (s < 0) s = 0;
  long t = (long)s;
  char b[48];
  if (t >= 3600) snprintf(b, sizeof b, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
  else snprintf(b, sizeof b, "%02ld:%02ld", t / 60, t % 60);
  std::string r = b;
  if (tenths) r += "." + std::to_string((int)((s - t) * 10) % 10);
  return r;
}

double parse_duration(const std::string &text) {
  std::string t = fld::lower(fld::trim(text));
  if (t.empty()) return -1;
  if (t.find(':') != std::string::npos) {
    std::vector<std::string> parts = fld::split(t, ':', true);
    if (parts.size() > 3) return -1;
    double total = 0;
    for (const std::string &p : parts) {
      if (p.empty() || p.find_first_not_of("0123456789") != std::string::npos) return -1;
      total = total * 60 + atoi(p.c_str());
    }
    return total;
  }
  // 1h30m, 90s, 5m, or a plain number of minutes
  double total = 0, num = -1;
  std::string digits;
  bool unit = false;
  for (char c : t) {
    if (isdigit((unsigned char)c) || c == '.') {
      digits += c;
    } else if (c == 'h' || c == 'm' || c == 's') {
      if (digits.empty()) return -1;
      num = atof(digits.c_str());
      total += num * (c == 'h' ? 3600 : c == 'm' ? 60 : 1);
      digits.clear();
      unit = true;
    } else if (c != ' ') {
      return -1;
    }
  }
  if (!digits.empty()) total += atof(digits.c_str()) * (unit ? 1 : 60);
  return total;
}

bool parse_hhmm(const std::string &s, int &h, int &m) {
  int a, b;
  char extra;
  if (sscanf(fld::trim(s).c_str(), "%d:%d%c", &a, &b, &extra) != 2) return false;
  if (a < 0 || a > 23 || b < 0 || b > 59) return false;
  h = a;
  m = b;
  return true;
}

time_t next_ring(const Alarm &a, time_t now) {
  if (!a.enabled) return 0;
  if (a.snoozed_until > now) return a.snoozed_until;
  struct tm tm;
  localtime_r(&now, &tm);
  for (int d = 0; d < 8; d++) {
    struct tm c = tm;
    c.tm_mday += d;
    c.tm_hour = a.hour;
    c.tm_min = a.minute;
    c.tm_sec = 0;
    c.tm_isdst = -1;
    time_t t = mktime(&c);
    if (t <= now) continue;
    if (a.days == 0 || (a.days & (1 << c.tm_wday))) return t;
  }
  return 0;
}

std::string days_text(int days) {
  if (days == 0) return "Once";
  if (days == 0x7f) return "Every day";
  if (days == 0x3e) return "Weekdays";
  if (days == 0x41) return "Weekends";
  static const char *n[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  std::string o;
  // Monday first
  for (int i : {1, 2, 3, 4, 5, 6, 0})
    if (days & (1 << i)) o += (o.empty() ? "" : " ") + std::string(n[i]);
  return o;
}

std::string alarms_to_ini(const std::vector<Alarm> &alarms) {
  fld::Ini ini;
  for (const Alarm &a : alarms) {
    std::string g = "alarm:" + a.id;
    char t[8];
    snprintf(t, sizeof t, "%02d:%02d", a.hour, a.minute);
    ini.set(g, "Time", t);
    ini.set_int(g, "Days", a.days);
    ini.set(g, "Label", a.label);
    ini.set_bool(g, "Enabled", a.enabled);
    if (a.snoozed_until) ini.set(g, "SnoozedUntil", std::to_string((long long)a.snoozed_until));
    if (a.last_rang) ini.set(g, "LastRang", std::to_string((long long)a.last_rang));
  }
  return ini.to_string();
}

std::vector<Alarm> alarms_from_ini(const std::string &text) {
  fld::Ini ini;
  ini.parse(text);
  std::vector<Alarm> out;
  for (const std::string &g : ini.groups()) {
    if (!fld::starts_with(g, "alarm:")) continue;
    Alarm a;
    a.id = g.substr(6);
    if (!parse_hhmm(ini.get(g, "Time", "07:00"), a.hour, a.minute)) continue;
    a.days = ini.get_int(g, "Days", 0) & 0x7f;
    a.label = ini.get(g, "Label", "");
    a.enabled = ini.get_bool(g, "Enabled", true);
    a.snoozed_until = (time_t)atoll(ini.get(g, "SnoozedUntil", "0").c_str());
    a.last_rang = (time_t)atoll(ini.get(g, "LastRang", "0").c_str());
    out.push_back(a);
  }
  std::sort(out.begin(), out.end(), [](const Alarm &a, const Alarm &b) {
    return a.hour * 60 + a.minute < b.hour * 60 + b.minute;
  });
  return out;
}

std::vector<std::string> parse_zone_tab(const std::string &text) {
  std::vector<std::string> out;
  for (const std::string &line : fld::split(text, '\n')) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> f = fld::split(line, '\t', true);
    if (f.size() >= 3 && !f[2].empty()) out.push_back(f[2]);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

std::string city_of(const std::string &zone) {
  std::string c = zone.substr(zone.rfind('/') == std::string::npos ? 0 : zone.rfind('/') + 1);
  std::replace(c.begin(), c.end(), '_', ' ');
  return c;
}

namespace {

// a zone's offset from UTC over a stretch of time where it does not change
struct ZoneSpan {
  time_t from = 0, until = 0;
  long gmtoff = 0;
  int isdst = 0;
  std::string abbr;
};

// Time zones change their offset on a quarter hour of UTC (all of tzdata's
// transitions do), so one lookup holds until the next quarter hour.
constexpr time_t kSpan = 15 * 60;

struct tm slow_time_in(const std::string &zone, time_t t) {
  struct tm tm;
  // TZ for a moment (the program has one thread doing this)
  const char *old = getenv("TZ");
  std::string saved = old ? old : "";
  setenv("TZ", (":" + zone).c_str(), 1);
  tzset();
  localtime_r(&t, &tm);
  if (old) setenv("TZ", saved.c_str(), 1);
  else unsetenv("TZ");
  tzset();
  return tm;
}

} // namespace

struct tm time_in(const std::string &zone, time_t t) {
  struct tm tm;
  if (zone.empty()) {
    localtime_r(&t, &tm);
    return tm;
  }
  // reading the zone's file again and again for every clock shown costs a
  // lot of CPU, so remember its offset until it may change
  static std::map<std::string, ZoneSpan> spans;
  ZoneSpan &sp = spans[zone];
  if (t < sp.from || t >= sp.until) {
    tm = slow_time_in(zone, t);
    sp.from = t - ((t % kSpan) + kSpan) % kSpan;
    sp.until = sp.from + kSpan;
    sp.gmtoff = tm.tm_gmtoff;
    sp.isdst = tm.tm_isdst;
    sp.abbr = tm.tm_zone ? tm.tm_zone : "";
    return tm;
  }
  time_t local = t + sp.gmtoff;
  gmtime_r(&local, &tm);
  tm.tm_gmtoff = sp.gmtoff;
  tm.tm_isdst = sp.isdst;
  tm.tm_zone = sp.abbr.c_str();
  return tm;
}

std::string offset_text(const std::string &zone, time_t t) {
  long here = time_in("", t).tm_gmtoff, there = time_in(zone, t).tm_gmtoff;
  long d = there - here;
  if (d == 0) return "same time";
  char b[32];
  long a = std::labs(d);
  if (a % 3600) snprintf(b, sizeof b, "%c%ld:%02ld", d < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
  else snprintf(b, sizeof b, "%c%ld h", d < 0 ? '-' : '+', a / 3600);
  return b;
}

} // namespace clk
