#include "playlist_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>

namespace media {

namespace {

std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

std::string lower(std::string s) {
  for (char &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

bool is_url(const std::string &s) {
  size_t c = s.find("://");
  if (c == std::string::npos || c == 0) return false;
  for (size_t i = 0; i < c; i++)
    if (!isalnum((unsigned char)s[i]) && s[i] != '+' && s[i] != '-' && s[i] != '.') return false;
  return true;
}

std::string decode(const std::string &s) {
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else {
      r += s[i];
    }
  }
  return r;
}

// a location as written in a playlist: file:// URIs and relative paths
std::string location(const std::string &raw, const std::string &dir) {
  std::string l = trim(raw);
  if (l.compare(0, 7, "file://") == 0) {
    l = l.substr(7);
    if (l.compare(0, 9, "localhost") == 0) l = l.substr(9);
    return decode(l);
  }
  if (is_url(l)) return l;
  // Windows playlists: backslashes
  std::replace(l.begin(), l.end(), '\\', '/');
  if (!l.empty() && l[0] != '/' && !dir.empty()) l = dir + "/" + l;
  return l;
}

} // namespace

bool is_playlist(const std::string &path) {
  std::string p = lower(path);
  for (const char *e : {".m3u", ".m3u8", ".pls"})
    if (p.size() > strlen(e) && p.compare(p.size() - strlen(e), strlen(e), e) == 0) return true;
  return false;
}

std::vector<Entry> parse_playlist(const std::string &text, const std::string &dir, bool pls) {
  std::vector<Entry> out;
  std::vector<std::string> lines;
  size_t i = 0;
  if (text.compare(0, 3, "\xef\xbb\xbf") == 0) i = 3;
  while (i <= text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    lines.push_back(trim(text.substr(i, e - i)));
    i = e + 1;
  }
  if (pls) {
    // [playlist] FileN= TitleN= LengthN=
    std::map<int, Entry> by_n;
    for (const std::string &l : lines) {
      size_t eq = l.find('=');
      if (eq == std::string::npos) continue;
      std::string k = lower(trim(l.substr(0, eq))), v = trim(l.substr(eq + 1));
      for (const char *key : {"file", "title", "length"}) {
        size_t kl = strlen(key);
        if (k.compare(0, kl, key) != 0 || k.size() == kl) continue;
        std::string num = k.substr(kl);
        if (num.find_first_not_of("0123456789") != std::string::npos) continue;
        Entry &en = by_n[atoi(num.c_str())];
        if (key[0] == 'f') en.location = location(v, dir);
        else if (key[0] == 't') en.title = v;
        else en.length = atof(v.c_str());
      }
    }
    for (auto &p : by_n)
      if (!p.second.location.empty()) out.push_back(p.second);
    return out;
  }
  Entry pending;
  for (const std::string &l : lines) {
    if (l.empty()) continue;
    if (l[0] == '#') {
      // #EXTINF:<seconds>[ attributes],<title>
      if (l.compare(0, 8, "#EXTINF:") == 0) {
        size_t comma = l.find(',');
        pending.length = atof(l.substr(8).c_str());
        if (pending.length <= 0) pending.length = -1;
        pending.title = comma == std::string::npos ? "" : trim(l.substr(comma + 1));
      }
      continue;
    }
    pending.location = location(l, dir);
    out.push_back(pending);
    pending = Entry();
  }
  return out;
}

std::string write_m3u(const std::vector<Entry> &entries) {
  std::string o = "#EXTM3U\n";
  for (const Entry &e : entries) {
    if (!e.title.empty() || e.length > 0)
      o += "#EXTINF:" + std::to_string(e.length > 0 ? (long)std::lround(e.length) : -1) + "," + e.title + "\n";
    o += e.location + "\n";
  }
  return o;
}

std::string write_pls(const std::vector<Entry> &entries) {
  std::string o = "[playlist]\n";
  for (size_t i = 0; i < entries.size(); i++) {
    std::string n = std::to_string(i + 1);
    o += "File" + n + "=" + entries[i].location + "\n";
    if (!entries[i].title.empty()) o += "Title" + n + "=" + entries[i].title + "\n";
    o += "Length" + n + "=" + std::to_string(entries[i].length > 0 ? (long)std::lround(entries[i].length) : -1) + "\n";
  }
  o += "NumberOfEntries=" + std::to_string(entries.size()) + "\nVersion=2\n";
  return o;
}

std::vector<int> shuffle_order(int n, int first, unsigned seed) {
  std::vector<int> v;
  for (int i = 0; i < n; i++)
    if (i != first) v.push_back(i);
  std::mt19937 rng(seed);
  std::shuffle(v.begin(), v.end(), rng);
  if (first >= 0 && first < n) v.insert(v.begin(), first);
  return v;
}

Repeat next_repeat(Repeat r) { return r == Repeat::Off ? Repeat::All : r == Repeat::All ? Repeat::One : Repeat::Off; }

int gapless_next(const std::vector<int> &order, int cur, Repeat repeat, const std::vector<bool> &local) {
  int n = step(order, cur, 1, repeat);
  return n >= 0 && n < (int)local.size() && local[(size_t)n] ? n : -1;
}

int step(const std::vector<int> &order, int cur, int dir, Repeat repeat) {
  if (order.empty()) return -1;
  if (repeat == Repeat::One && cur >= 0) return cur;
  auto it = std::find(order.begin(), order.end(), cur);
  int pos = it == order.end() ? (dir > 0 ? -1 : (int)order.size()) : (int)(it - order.begin());
  int next = pos + (dir > 0 ? 1 : -1);
  if (next >= (int)order.size()) return repeat == Repeat::All ? order.front() : -1;
  if (next < 0) return repeat == Repeat::All ? order.back() : -1;
  return order[(size_t)next];
}

} // namespace media
