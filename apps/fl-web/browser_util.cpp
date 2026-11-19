#include "browser_util.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "fld/util.h"

namespace web {

const std::vector<SearchEngine> &search_engines() {
  static const std::vector<SearchEngine> e = {
      {"DuckDuckGo", "https://duckduckgo.com/?q=%s"},
      {"Startpage", "https://www.startpage.com/do/search?query=%s"},
      {"Brave Search", "https://search.brave.com/search?q=%s"},
      {"Google", "https://www.google.com/search?q=%s"},
      {"Bing", "https://www.bing.com/search?q=%s"},
      {"Wikipedia", "https://en.wikipedia.org/w/index.php?search=%s"},
  };
  return e;
}

std::string search_template(const std::string &n) {
  for (auto &e : search_engines())
    if (fld::lower(n) == fld::lower(e.name)) return e.url;
  if (n.find("%s") != std::string::npos && (fld::starts_with(n, "https://") || fld::starts_with(n, "http://")))
    return n;
  return search_engines()[0].url;
}

std::string encode_query(const std::string &q) {
  static const char *hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : q) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else if (c == ' ') out += '+';
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

namespace {

bool has_scheme(const std::string &t) {
  static const char *schemes[] = {"http://", "https://", "file://", "about:", "data:", "view-source:", "ftp://",
                                  "webkit://", "blob:"};
  std::string l = fld::lower(t);
  for (const char *s : schemes)
    if (fld::starts_with(l, s)) return true;
  return false;
}

bool is_ipv4(const std::string &h) {
  int parts = 0, n = 0, digits = 0;
  for (char c : h + ".") {
    if (c == '.') {
      if (!digits || n > 255) return false;
      parts++;
      n = digits = 0;
    } else if (isdigit((unsigned char)c)) {
      n = n * 10 + (c - '0');
      if (++digits > 3) return false;
    } else return false;
  }
  return parts == 4;
}

// "example.com", "example.com/path", "localhost:8080", "192.168.1.1", "[::1]:80"
bool looks_like_host(const std::string &t) {
  if (t.find(' ') != std::string::npos || t.empty()) return false;
  std::string host = t.substr(0, t.find_first_of("/?#"));
  if (host.empty()) return false;
  if (host[0] == '[') return host.find(']') != std::string::npos; // IPv6
  size_t colon = host.rfind(':');
  if (colon != std::string::npos) {
    std::string port = host.substr(colon + 1);
    if (port.empty() || !std::all_of(port.begin(), port.end(), [](char c) { return isdigit((unsigned char)c); }))
      return false;
    host = host.substr(0, colon);
  }
  if (fld::lower(host) == "localhost" || is_ipv4(host)) return true;
  size_t dot = host.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 2 > host.size()) return false;
  // the top level domain: letters only (so "3.14" and "file.txt"-like words with digits fail)
  std::string tld = host.substr(dot + 1);
  if (!std::all_of(tld.begin(), tld.end(), [](char c) { return isalpha((unsigned char)c); })) return false;
  for (char c : host)
    if (!(isalnum((unsigned char)c) || c == '-' || c == '.' || (unsigned char)c >= 0x80)) return false;
  return true;
}

} // namespace

std::string url_from_input(const std::string &text, const std::string &tmpl) {
  std::string t = fld::trim(text);
  if (t.empty()) return "";
  if (has_scheme(t)) return t;
  if (t[0] == '/') return "file://" + fld::uri_encode_path(t);
  if (t[0] == '~' && (t.size() == 1 || t[1] == '/')) return "file://" + fld::uri_encode_path(fld::expand_tilde(t));
  if (looks_like_host(t)) {
    bool local = fld::starts_with(fld::lower(t), "localhost") || is_ipv4(t.substr(0, t.find_first_of(":/?#")));
    return (local ? "http://" : "https://") + t;
  }
  std::string tm = tmpl.empty() ? search_engines()[0].url : tmpl;
  size_t p = tm.find("%s");
  if (p == std::string::npos) return tm + encode_query(t);
  return tm.substr(0, p) + encode_query(t) + tm.substr(p + 2);
}

std::string url_host(const std::string &url) {
  size_t s = url.find("://");
  if (s == std::string::npos) return "";
  std::string rest = url.substr(s + 3);
  std::string host = rest.substr(0, rest.find_first_of("/?#"));
  size_t at = host.rfind('@');
  if (at != std::string::npos) host = host.substr(at + 1);
  if (!host.empty() && host[0] != '[') host = host.substr(0, host.find(':'));
  return host;
}

std::string fallback_title(const std::string &url) {
  if (fld::starts_with(url, "file://")) {
    std::string p = fld::uri_decode(url.substr(7));
    return p == "/" ? p : fld::basename(p);
  }
  std::string h = url_host(url);
  if (fld::starts_with(h, "www.")) h = h.substr(4);
  return h.empty() ? url : h;
}

std::string one_line(const std::string &s) {
  std::string o;
  for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
  return o;
}

std::vector<Bookmark> load_bookmarks(const std::string &path) {
  std::vector<Bookmark> out;
  std::string data;
  if (!fld::read_file(path, data)) return out;
  for (auto &line : fld::split(data, '\n')) {
    if (line.empty()) continue;
    size_t tab = line.find('\t');
    Bookmark b;
    b.url = line.substr(0, tab);
    b.title = tab == std::string::npos ? "" : line.substr(tab + 1);
    if (!b.url.empty()) out.push_back(b);
  }
  return out;
}

bool save_bookmarks(const std::string &path, const std::vector<Bookmark> &bs) {
  std::string data;
  for (auto &b : bs) data += one_line(b.url) + "\t" + one_line(b.title) + "\n";
  fld::mkdir_p(fld::dirname(path));
  return fld::write_file(path, data);
}

static std::string history_line(const HistoryEntry &e) {
  return std::to_string((long long)e.time) + "\t" + one_line(e.url) + "\t" + one_line(e.title) + "\n";
}

std::vector<HistoryEntry> load_history(const std::string &path) {
  std::vector<HistoryEntry> out;
  std::string data;
  if (!fld::read_file(path, data)) return out;
  for (auto &line : fld::split(data, '\n')) {
    size_t t1 = line.find('\t');
    if (t1 == std::string::npos) continue;
    size_t t2 = line.find('\t', t1 + 1);
    HistoryEntry e;
    e.time = (time_t)atoll(line.substr(0, t1).c_str());
    e.url = line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1);
    e.title = t2 == std::string::npos ? "" : line.substr(t2 + 1);
    if (!e.url.empty()) out.push_back(e);
  }
  return out;
}

bool add_history(const std::string &path, const HistoryEntry &e, size_t keep) {
  fld::mkdir_p(fld::dirname(path));
  FILE *f = fopen(path.c_str(), "a");
  if (!f) return false;
  std::string l = history_line(e);
  fwrite(l.data(), 1, l.size(), f);
  long size = ftell(f);
  fclose(f);
  // compact now and then: the latest visit of each URL, newest `keep`
  if (size > (long)(keep * 2 * 120)) {
    auto all = load_history(path);
    if (all.size() > keep * 2) {
      std::map<std::string, size_t> last;
      for (size_t i = 0; i < all.size(); i++) last[all[i].url] = i;
      std::vector<HistoryEntry> kept;
      for (size_t i = 0; i < all.size(); i++)
        if (last[all[i].url] == i) kept.push_back(all[i]);
      if (kept.size() > keep) kept.erase(kept.begin(), kept.end() - (long)keep);
      std::string data;
      for (auto &k : kept) data += history_line(k);
      return fld::write_file(path, data);
    }
  }
  return true;
}

std::vector<HistoryEntry> history_view(const std::vector<HistoryEntry> &all, const std::string &filter, size_t max) {
  std::vector<HistoryEntry> out;
  std::map<std::string, bool> seen;
  std::string f = fld::lower(filter);
  for (auto it = all.rbegin(); it != all.rend() && out.size() < max; ++it) {
    if (seen.count(it->url)) continue;
    seen[it->url] = true;
    if (!f.empty() && fld::lower(it->url).find(f) == std::string::npos &&
        fld::lower(it->title).find(f) == std::string::npos)
      continue;
    out.push_back(*it);
  }
  return out;
}

std::vector<Bookmark> suggestions(const std::string &text, const std::vector<Bookmark> &bookmarks,
                                  const std::vector<HistoryEntry> &history, size_t max) {
  std::vector<Bookmark> out;
  std::string t = fld::lower(fld::trim(text));
  if (t.empty()) return out;
  auto matches = [&](const std::string &url, const std::string &title) {
    std::string u = fld::lower(url);
    // ignore the scheme and www. when matching the start
    for (const char *p : {"https://", "http://"})
      if (fld::starts_with(u, p)) u = u.substr(strlen(p));
    if (fld::starts_with(u, "www.")) u = u.substr(4);
    return u.find(t) != std::string::npos || fld::lower(title).find(t) != std::string::npos;
  };
  auto have = [&](const std::string &url) {
    return std::any_of(out.begin(), out.end(), [&](const Bookmark &b) { return b.url == url; });
  };
  for (auto &b : bookmarks)
    if (out.size() < max && matches(b.url, b.title) && !have(b.url)) out.push_back(b);
  // history: by number of visits, then recency
  std::map<std::string, std::pair<int, size_t>> visits; // url -> count, last index
  std::map<std::string, std::string> titles;
  for (size_t i = 0; i < history.size(); i++) {
    auto &v = visits[history[i].url];
    v.first++;
    v.second = i;
    if (!history[i].title.empty()) titles[history[i].url] = history[i].title;
  }
  std::vector<std::pair<std::string, std::pair<int, size_t>>> ranked(visits.begin(), visits.end());
  std::sort(ranked.begin(), ranked.end(), [](auto &a, auto &b) {
    if (a.second.first != b.second.first) return a.second.first > b.second.first;
    return a.second.second > b.second.second;
  });
  for (auto &r : ranked) {
    if (out.size() >= max) break;
    if (matches(r.first, titles[r.first]) && !have(r.first)) out.push_back({r.first, titles[r.first]});
  }
  return out;
}

} // namespace web
