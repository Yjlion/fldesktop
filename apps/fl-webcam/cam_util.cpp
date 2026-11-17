#include "cam_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <unistd.h>

namespace cam {

std::vector<Mode> best_modes(std::vector<Mode> modes) {
  std::stable_sort(modes.begin(), modes.end(), [](const Mode &a, const Mode &b) {
    long aa = (long)a.w * a.h, bb = (long)b.w * b.h;
    if (aa != bb) return aa > bb;
    if (a.w != b.w) return a.w > b.w;
    // same size: the faster one; equal speed: raw (no decoding)
    if (std::fabs(a.fps - b.fps) > 0.5) return a.fps > b.fps;
    return a.format == "raw" && b.format != "raw";
  });
  std::vector<Mode> out;
  for (auto &m : modes) {
    if (m.w <= 0 || m.h <= 0) continue;
    bool dup = false;
    for (auto &o : out) dup |= o.w == m.w && o.h == m.h;
    if (!dup) out.push_back(m);
  }
  return out;
}

std::string mode_label(const Mode &m) {
  char b[64];
  if (m.fps > 0) snprintf(b, sizeof b, "%d×%d (%g fps)", m.w, m.h, std::round(m.fps * 10) / 10);
  else snprintf(b, sizeof b, "%d×%d", m.w, m.h);
  return b;
}

size_t default_mode(const std::vector<Mode> &modes) {
  for (size_t i = 0; i < modes.size(); i++)
    if (modes[i].w <= 1920 && modes[i].h <= 1080 && (modes[i].fps <= 0 || modes[i].fps >= 24)) return i;
  return 0;
}

std::string capture_path(const std::string &dir, const std::string &ext, time_t t) {
  char stamp[40];
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(stamp, sizeof stamp, "%Y-%m-%d-%H-%M-%S", &tm);
  std::string base = dir + "/Webcam-" + stamp;
  std::string p = base + "." + ext;
  for (int n = 2; access(p.c_str(), F_OK) == 0; n++) p = base + "-" + std::to_string(n) + "." + ext;
  return p;
}

std::string format_duration(double secs) {
  if (!(secs >= 0)) secs = 0;
  long s = (long)secs;
  char b[32];
  if (s >= 3600) snprintf(b, sizeof b, "%ld:%02ld:%02ld", s / 3600, s / 60 % 60, s % 60);
  else snprintf(b, sizeof b, "%02ld:%02ld", s / 60, s % 60);
  return b;
}

} // namespace cam
