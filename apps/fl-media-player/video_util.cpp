#include "video_util.h"
#include "fld/ini.h"
#include "fld/util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

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

std::string find_subtitle(const std::string &video_path) {
  if (is_url(video_path)) return "";
  size_t dot = video_path.rfind('.');
  size_t slash = video_path.rfind('/');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
  std::string base = video_path.substr(0, dot);
  static const char *exts[] = {"srt", "ass", "ssa", "vtt", "sub"};
  for (const char *e : exts) {
    std::string p = base + "." + e;
    if (fld::file_exists(p)) return p;
  }
  // film.en.srt and the like
  std::string dir = slash == std::string::npos ? "." : video_path.substr(0, slash);
  std::string stem = base.substr(slash == std::string::npos ? 0 : slash + 1);
  std::vector<std::string> names = fld::list_dir(dir, false);
  std::sort(names.begin(), names.end());
  for (auto &n : names)
    if (n.size() > stem.size() + 1 && n.compare(0, stem.size() + 1, stem + ".") == 0 && is_subtitle(n))
      return dir + "/" + n;
  return "";
}

std::string to_uri(const std::string &s) {
  if (is_url(s)) return s;
  static const char *hex = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char c : s) {
    if (isalnum(c) || strchr("/-_.~!$&'()*+,;=:@", c)) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

bool needs_resolver(const std::string &url) {
  if (!is_url(url) || url.compare(0, 4, "http") != 0) return false;
  std::string host = url.substr(url.find("://") + 3);
  host = host.substr(0, host.find('/'));
  for (auto &c : host) c = (char)tolower((unsigned char)c);
  static const char *sites[] = {"youtube.com", "youtu.be", "vimeo.com", "dailymotion.com", "twitch.tv",
                                "peertube", "odysee.com", "soundcloud.com", "bilibili.com"};
  for (const char *s : sites)
    if (host.find(s) != std::string::npos) return true;
  return false;
}

std::string resume_key(const std::string &location) {
  // FNV-1a, 64 bit
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : location) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char buf[24];
  snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
  return buf;
}

double resume_get(const fld::Ini &ini, const std::string &location) {
  std::string k = resume_key(location);
  // a hash collision must not resume another file
  if (ini.get(k, "Location") != location) return 0;
  double p = ini.get_double(k, "Position", 0);
  return p > 0 ? p : 0;
}

void resume_put(fld::Ini &ini, const std::string &location, double pos, double duration) {
  std::string k = resume_key(location);
  bool near_end = duration > 0 && pos > duration - 10;
  if (!(pos >= 5) || near_end) {
    ini.remove_group(k);
    return;
  }
  char buf[32];
  snprintf(buf, sizeof buf, "%.2f", pos);
  ini.set(k, "Location", location);
  ini.set(k, "Position", buf);
}

} // namespace video
