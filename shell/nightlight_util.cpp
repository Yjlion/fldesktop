// fl-nightlight helpers (see nightlight_util.h)
#include "nightlight_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace nightlight {

namespace {

// Tanner Helland's fit of the black body colour, in 0..255
void kelvin_to_rgb255(double k, double *r, double *g, double *b) {
  double t = k / 100.0;
  if (t <= 66) {
    *r = 255;
    *g = 99.4708025861 * std::log(t) - 161.1195681661;
  } else {
    *r = 329.698727446 * std::pow(t - 60, -0.1332047592);
    *g = 288.1221695283 * std::pow(t - 60, -0.0755148492);
  }
  if (t >= 66) *b = 255;
  else if (t <= 19) *b = 0;
  else *b = 138.5177312231 * std::log(t - 10) - 305.0447927307;
  auto clamp = [](double v) { return std::max(0.0, std::min(255.0, v)); };
  *r = clamp(*r);
  *g = clamp(*g);
  *b = clamp(*b);
}

constexpr double kPi = 3.14159265358979323846;
double rad(double d) { return d * kPi / 180.0; }
double deg(double r) { return r * 180.0 / kPi; }

} // namespace

Rgb temperature_rgb(int kelvin) {
  kelvin = std::max(1000, std::min(10000, kelvin));
  double r, g, b, r0, g0, b0;
  kelvin_to_rgb255(kelvin, &r, &g, &b);
  kelvin_to_rgb255(6500, &r0, &g0, &b0);
  Rgb c;
  c.r = std::min(1.0, r / r0);
  c.g = std::min(1.0, g / g0);
  c.b = std::min(1.0, b / b0);
  return c;
}

std::vector<uint16_t> gamma_ramp(uint32_t size, const Rgb &c) {
  std::vector<uint16_t> ramp((size_t)size * 3);
  for (uint32_t i = 0; i < size; i++) {
    double v = size > 1 ? (double)i / (size - 1) : 1.0;
    ramp[i] = (uint16_t)std::lround(65535.0 * v * c.r);
    ramp[size + i] = (uint16_t)std::lround(65535.0 * v * c.g);
    ramp[2 * size + i] = (uint16_t)std::lround(65535.0 * v * c.b);
  }
  return ramp;
}

// NOAA's simplified sunrise equation (zenith 90.833 degrees)
bool sun_times(int doy, double lat, double lon, int utc_offset_min, int *sunrise, int *sunset, bool *polar_day) {
  double gamma = 2 * kPi / 365.0 * (doy - 1);
  double eqtime = 229.18 * (0.000075 + 0.001868 * std::cos(gamma) - 0.032077 * std::sin(gamma) -
                            0.014615 * std::cos(2 * gamma) - 0.040849 * std::sin(2 * gamma));
  double decl = 0.006918 - 0.399912 * std::cos(gamma) + 0.070257 * std::sin(gamma) - 0.006758 * std::cos(2 * gamma) +
                0.000907 * std::sin(2 * gamma) - 0.002697 * std::cos(3 * gamma) + 0.00148 * std::sin(3 * gamma);
  double cos_ha = std::cos(rad(90.833)) / (std::cos(rad(lat)) * std::cos(decl)) - std::tan(rad(lat)) * std::tan(decl);
  if (cos_ha > 1 || cos_ha < -1) {
    if (polar_day) *polar_day = cos_ha < -1;
    return false;
  }
  double ha = deg(std::acos(cos_ha));
  auto wrap = [](double m) {
    int v = (int)std::lround(m) % 1440;
    return v < 0 ? v + 1440 : v;
  };
  *sunrise = wrap(720 - 4 * (lon + ha) - eqtime + utc_offset_min);
  *sunset = wrap(720 - 4 * (lon - ha) - eqtime + utc_offset_min);
  return true;
}

bool parse_iso6709(const std::string &s, double *lat, double *lon) {
  // +DDMM[SS]+DDDMM[SS]
  size_t split = s.find_first_of("+-", 1);
  if (s.empty() || (s[0] != '+' && s[0] != '-') || split == std::string::npos) return false;
  auto conv = [](const std::string &p, int deg_digits, double *out) {
    std::string d = p.substr(1);
    if ((int)d.size() != deg_digits + 2 && (int)d.size() != deg_digits + 4) return false;
    for (char c : d)
      if (c < '0' || c > '9') return false;
    double v = std::atoi(d.substr(0, deg_digits).c_str()) + std::atoi(d.substr(deg_digits, 2).c_str()) / 60.0;
    if ((int)d.size() == deg_digits + 4) v += std::atoi(d.substr(deg_digits + 2, 2).c_str()) / 3600.0;
    *out = p[0] == '-' ? -v : v;
    return true;
  };
  return conv(s.substr(0, split), 2, lat) && conv(s.substr(split), 3, lon);
}

bool zone_coordinates(const std::string &tab_path, const std::string &zone, double *lat, double *lon) {
  std::ifstream f(tab_path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ls(line);
    std::string codes, coords, name;
    if (!std::getline(ls, codes, '\t') || !std::getline(ls, coords, '\t') || !std::getline(ls, name, '\t')) continue;
    if (name == zone) return parse_iso6709(coords, lat, lon);
  }
  return false;
}

int parse_hhmm(const std::string &s) {
  int h, m;
  char extra;
  if (std::sscanf(s.c_str(), "%d:%d%c", &h, &m, &extra) != 2) return -1;
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

double night_amount(int now, int start, int end, int fade) {
  auto since = [](int from, int t) { return ((t - from) % 1440 + 1440) % 1440; };
  int len = since(start, end); // night length
  if (len == 0) return 0;
  int t = since(start, now);   // minutes into the night
  if (t >= len) return 0;      // day
  fade = std::max(0, std::min(fade, len / 2));
  if (fade == 0) return 1;
  if (t < fade) return (double)t / fade;
  if (len - t < fade) return (double)(len - t) / fade;
  return 1;
}

int blend_temperature(int night_kelvin, double amount) {
  amount = std::max(0.0, std::min(1.0, amount));
  return (int)std::lround(6500 + (night_kelvin - 6500) * amount);
}

} // namespace nightlight
