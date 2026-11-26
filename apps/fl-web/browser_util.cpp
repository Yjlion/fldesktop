#include "browser_util.h"
#include <sstream>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#include "fld/json.h"
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

// ---- saved logins -------------------------------------------------------------

const char *const kLoginSchema = "org.fldesktop.Web.Login";

std::string login_origin(const std::string &url) {
  size_t se = url.find("://");
  if (se == std::string::npos) return "";
  std::string scheme = fld::lower(url.substr(0, se));
  if (scheme != "https" && scheme != "http") return "";
  size_t end = url.find_first_of("/?#", se + 3);
  std::string host = fld::lower(url.substr(se + 3, end == std::string::npos ? std::string::npos : end - se - 3));
  size_t at = host.rfind('@');
  if (at != std::string::npos) host = host.substr(at + 1); // no user:password@
  if (host.empty()) return "";
  // the default port is left out
  std::string def = scheme == "https" ? ":443" : ":80";
  if (host.size() > def.size() && host.compare(host.size() - def.size(), def.size(), def) == 0)
    host.resize(host.size() - def.size());
  return scheme + "://" + host;
}

const char *login_capture_script() {
  return R"JS((function () {
  if (window.top !== window || !window.webkit || !window.webkit.messageHandlers.flwebLogin) return;
  var last = '';
  function grab(form) {
    if (!form || !form.querySelector) return;
    var pw = form.querySelector('input[type=password]');
    if (!pw || !pw.value) return;
    var user = '', ins = form.querySelectorAll('input');
    for (var i = 0; i < ins.length && ins[i] !== pw; i++) {
      var t = (ins[i].type || 'text').toLowerCase();
      if ((t === 'text' || t === 'email' || t === 'tel') && ins[i].value) user = ins[i].value;
    }
    var m = JSON.stringify({origin: location.origin, user: user, pass: pw.value});
    if (m === last) return;
    last = m;
    window.webkit.messageHandlers.flwebLogin.postMessage(m);
  }
  document.addEventListener('submit', function (e) { grab(e.target); }, true);
  // pages that log in from a script: the button of a form with a password
  document.addEventListener('click', function (e) {
    var b = e.target && e.target.closest ? e.target.closest('button, input[type=submit]') : null;
    if (b && b.form) grab(b.form);
  }, true);
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Enter' && e.target && e.target.form) grab(e.target.form);
  }, true);
})();)JS";
}

bool parse_login_message(const std::string &json, std::string &origin, std::string &user, std::string &pass) {
  fld::Json j;
  if (!fld::Json::parse(json, j) || !j.is_object() || !j["pass"].is_string() || !j["origin"].is_string()) return false;
  origin = login_origin(j["origin"].as_string() + "/");
  user = j["user"].as_string();
  pass = j["pass"].as_string();
  return !origin.empty() && !pass.empty();
}

std::string login_fill_script(const std::string &user, const std::string &pass) {
  return "(function (u, p) {\n"
         "  var pw = document.querySelector('input[type=password]');\n"
         "  if (!pw || pw.value) return 0;\n"
         "  var f = pw.form || document, ins = f.querySelectorAll('input'), un = null;\n"
         "  for (var i = 0; i < ins.length && ins[i] !== pw; i++) {\n"
         "    var t = (ins[i].type || 'text').toLowerCase();\n"
         "    if (t === 'text' || t === 'email' || t === 'tel') un = ins[i];\n"
         "  }\n"
         "  function set(el, v) {\n"
         "    el.value = v;\n"
         "    el.dispatchEvent(new Event('input', {bubbles: true}));\n"
         "    el.dispatchEvent(new Event('change', {bubbles: true}));\n"
         "  }\n"
         "  if (un && u) set(un, u);\n"
         "  set(pw, p);\n"
         "  return 1;\n"
         "})(" + fld::Json(user).dump() + ", " + fld::Json(pass).dump() + ")";
}

std::vector<std::string> csv_record(const std::string &t, size_t &pos) {
  std::vector<std::string> f;
  std::string cur;
  bool quoted = false, any = false;
  for (; pos < t.size(); pos++) {
    char c = t[pos];
    any = true;
    if (quoted) {
      if (c == '"') {
        if (pos + 1 < t.size() && t[pos + 1] == '"') {
          cur += '"';
          pos++;
        } else {
          quoted = false;
        }
      } else {
        cur += c;
      }
    } else if (c == '"') {
      quoted = true;
    } else if (c == ',') {
      f.push_back(cur);
      cur.clear();
    } else if (c == '\n' || c == '\r') {
      if (c == '\r' && pos + 1 < t.size() && t[pos + 1] == '\n') pos++;
      pos++;
      break;
    } else {
      cur += c;
    }
  }
  if (any) f.push_back(cur);
  return f;
}

std::vector<CsvLogin> parse_password_csv(const std::string &text, std::string *err) {
  std::vector<CsvLogin> out;
  size_t pos = 0;
  if (text.compare(0, 3, "\xef\xbb\xbf") == 0) pos = 3; // a byte order mark
  std::vector<std::string> head = csv_record(text, pos);
  int iu = -1, in = -1, ip = -1;
  for (size_t i = 0; i < head.size(); i++) {
    std::string h = head[i];
    for (auto &ch : h) ch = (char)tolower((unsigned char)ch);
    if (h == "url" || h == "origin" || h == "login_uri") iu = (int)i;
    else if (h == "username" || h == "login_username") in = (int)i;
    else if (h == "password" || h == "login_password") ip = (int)i;
  }
  if (iu < 0 || ip < 0) {
    if (err) *err = "This is not a password export (it needs url and password columns).";
    return out;
  }
  while (pos < text.size()) {
    std::vector<std::string> r = csv_record(text, pos);
    if ((int)r.size() <= std::max(iu, ip)) continue;
    std::string origin = login_origin(r[(size_t)iu]);
    if (origin.empty() || r[(size_t)ip].empty()) continue;
    out.push_back({origin, in >= 0 && in < (int)r.size() ? r[(size_t)in] : "", r[(size_t)ip]});
  }
  return out;
}

bool parse_userscript(const std::string &text, UserScript &out) {
  out = UserScript();
  size_t a = text.find("// ==UserScript==");
  size_t b = text.find("// ==/UserScript==");
  if (a == std::string::npos || b == std::string::npos || b < a) return false;
  std::istringstream in(text.substr(a, b - a));
  std::string line;
  while (std::getline(in, line)) {
    size_t at = line.find('@');
    if (at == std::string::npos) continue;
    std::string rest = line.substr(at + 1);
    size_t sp = rest.find_first_of(" \t");
    std::string key = rest.substr(0, sp), val = sp == std::string::npos ? "" : rest.substr(sp);
    size_t v0 = val.find_first_not_of(" \t"), v1 = val.find_last_not_of(" \t\r");
    val = v0 == std::string::npos ? "" : val.substr(v0, v1 - v0 + 1);
    if (key == "name") out.name = val;
    else if (key == "match" || key == "include") {
      if (val == "*") val = "*://*/*";
      if (!val.empty()) out.matches.push_back(val);
    } else if (key == "exclude" || key == "exclude-match") {
      if (!val.empty()) out.excludes.push_back(val);
    } else if (key == "run-at") out.at_start = val == "document-start";
  }
  out.source = text;
  // without a @match it would run everywhere: only on purpose ("*")
  return !out.matches.empty();
}

} // namespace web
