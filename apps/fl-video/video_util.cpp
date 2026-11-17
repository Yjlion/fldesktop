#include "video_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace video {

std::string format_time(double s) {
  if (!(s >= 0) || std::isinf(s)) return "--:--";
  long t = (long)s;
  char b[32];
  if (t >= 3600) snprintf(b, sizeof b, "%ld:%02ld:%02ld", t / 3600, (t / 60) % 60, t % 60);
  else snprintf(b, sizeof b, "%ld:%02ld", t / 60, t % 60);
  return b;
}

bool is_url(const std::string &s) {
  size_t c = s.find("://");
  if (c == std::string::npos || c == 0) return false;
  for (size_t i = 0; i < c; i++)
    if (!isalnum((unsigned char)s[i]) && s[i] != '+' && s[i] != '-' && s[i] != '.') return false;
  return true;
}

static std::string uri_decode(const std::string &s) {
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      r += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
      i += 2;
    } else r += s[i];
  }
  return r;
}

std::vector<std::string> parse_uri_list(const std::string &text, const std::string &cwd) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    std::string line = text.substr(i, e - i);
    i = e + 1;
    while (!line.empty() && isspace((unsigned char)line.back())) line.pop_back();
    size_t a = 0;
    while (a < line.size() && isspace((unsigned char)line[a])) a++;
    line = line.substr(a);
    if (line.empty() || line[0] == '#') continue;
    if (line.compare(0, 7, "file://") == 0) {
      std::string p = line.substr(7);
      if (p.compare(0, 9, "localhost") == 0) p = p.substr(9);
      out.push_back(uri_decode(p));
    } else if (is_url(line)) {
      out.push_back(line);
    } else if (line[0] == '/') {
      out.push_back(line);
    } else {
      out.push_back(cwd + (cwd.empty() || cwd.back() == '/' ? "" : "/") + line);
    }
  }
  return out;
}

bool is_subtitle(const std::string &path) {
  size_t dot = path.rfind('.');
  if (dot == std::string::npos) return false;
  std::string ext = path.substr(dot + 1);
  for (auto &c : ext) c = (char)tolower((unsigned char)c);
  return ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt" || ext == "sub";
}

const std::vector<double> &speeds() {
  static const std::vector<double> v = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0, 4.0};
  return v;
}

double next_speed(double cur, int dir) {
  const auto &v = speeds();
  if (dir > 0) {
    for (double s : v)
      if (s > cur + 1e-6) return s;
    return v.back();
  }
  for (auto it = v.rbegin(); it != v.rend(); ++it)
    if (*it < cur - 1e-6) return *it;
  return v.front();
}

} // namespace video
