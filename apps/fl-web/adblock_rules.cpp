// fl-web: Adblock Plus filter lists to WebKit content blocker rules
//
// WebKit's url-filter is a small regular expression language: literals,
// '.', character classes, groups and the quantifiers * + ?, with '^' and '$'
// only at the ends (no alternation, no counted repetition). Filters that need
// more (regular expression filters, scriptlets, procedural cosmetic filters,
// $redirect, $csp, ...) are skipped.
#include "adblock_rules.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <set>

namespace web {

namespace {

// '^' in a filter: a separator (anything but a letter, digit or _-.%). WebKit
// has no "or the end of the address", so this is the separators URLs have.
const char *kSeparator = "[/:?=&]";
const char *kDomainStart = "^[^:]+://+([^:/]+\\.)?";
const size_t kSelectorsPerRule = 100;

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  for (;;) {
    size_t p = s.find(sep, start);
    out.push_back(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
    if (p == std::string::npos) break;
    start = p + 1;
  }
  return out;
}

void json_string(std::string &out, const std::string &s) {
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\t': out += "\\t"; break;
    case '\r': out += "\\r"; break;
    default:
      if (c < 0x20) {
        char buf[8];
        snprintf(buf, sizeof buf, "\\u%04x", c);
        out += buf;
      } else {
        out += (char)c;
      }
    }
  }
  out += '"';
}

void json_array(std::string &out, const std::vector<std::string> &v) {
  out += '[';
  for (size_t i = 0; i < v.size(); i++) {
    if (i) out += ',';
    json_string(out, v[i]);
  }
  out += ']';
}

struct Trigger {
  std::string url_filter = ".*";
  bool case_sensitive = false;
  std::vector<std::string> resource_types, load_type, load_context, if_domain, unless_domain;
};

std::string rule(const Trigger &t, const char *action, const std::string &selector = "") {
  std::string out = "{\"trigger\":{\"url-filter\":";
  json_string(out, t.url_filter);
  if (t.case_sensitive) out += ",\"url-filter-is-case-sensitive\":true";
  if (!t.resource_types.empty()) { out += ",\"resource-type\":"; json_array(out, t.resource_types); }
  if (!t.load_type.empty()) { out += ",\"load-type\":"; json_array(out, t.load_type); }
  if (!t.load_context.empty()) { out += ",\"load-context\":"; json_array(out, t.load_context); }
  // WebKit takes one of the two
  if (!t.if_domain.empty()) { out += ",\"if-domain\":"; json_array(out, t.if_domain); }
  else if (!t.unless_domain.empty()) { out += ",\"unless-domain\":"; json_array(out, t.unless_domain); }
  out += "},\"action\":{\"type\":\"";
  out += action;
  out += '"';
  if (!selector.empty()) { out += ",\"selector\":"; json_string(out, selector); }
  out += "}}";
  return out;
}

// splits "a.com,~b.com" (cosmetic) or "a.com|~b.com" ($domain=) into the
// sites a filter is for and the ones it is not; false when one cannot be used
bool domains(const std::string &list, char sep, std::vector<std::string> &yes, std::vector<std::string> &no) {
  for (const std::string &raw : split(list, sep)) {
    std::string d = trim(raw);
    if (d.empty()) continue;
    bool neg = d[0] == '~';
    std::string w = abp_domain(neg ? d.substr(1) : d);
    if (w.empty()) return false;
    (neg ? no : yes).push_back(w);
  }
  return true;
}

// element hiding WebKit cannot do: procedural and extended selectors
bool plain_selector(const std::string &sel) {
  static const char *bad[] = {":-abp-", ":has-text(", ":contains(", ":xpath(", ":upward(", ":remove(",
                              ":style(", ":matches-css", ":matches-path(", ":matches-attr(", ":matches-prop(",
                              ":min-text-length(", ":watch-attr(", ":others(", ":if(", ":if-not(",
                              ":nth-ancestor(", ":remove-attr(", ":remove-class(", "[-ext-", ":matches-media("};
  if (sel.empty() || sel[0] == '+' || sel.compare(0, 4, "+js(") == 0) return false;
  for (const char *b : bad)
    if (sel.find(b) != std::string::npos) return false;
  return true;
}

// the resource types of a network filter's options
const std::map<std::string, std::vector<std::string>> &type_names() {
  static const std::map<std::string, std::vector<std::string>> m = {
      {"script", {"script"}},
      {"image", {"image"}},
      {"stylesheet", {"style-sheet"}},
      {"css", {"style-sheet"}},
      {"font", {"font"}},
      {"media", {"media"}},
      {"subdocument", {"document"}},
      {"frame", {"document"}},
      {"xmlhttprequest", {"raw"}},
      {"xhr", {"raw"}},
      {"websocket", {"raw"}},
      {"other", {"raw"}},
      {"ping", {"ping"}},
      {"beacon", {"ping"}},
      {"popup", {"popup"}},
      {"object", {}}, // plugins: there are none
      {"webrtc", {}},
  };
  return m;
}

const std::vector<std::string> kSubresources = {"image", "style-sheet", "script", "font", "media", "raw",
                                                "svg-document", "ping"};

struct Network {
  std::vector<std::string> block, except, allow_sites;
};

struct Cosmetic {
  std::vector<std::string> generic;        // ##sel, in list order
  std::set<std::string> generic_seen;
  std::set<std::string> generic_exempt;    // #@#sel for every site
  std::map<std::string, std::vector<std::string>> generic_exempt_on; // #@#sel for some sites
  // a.com,b.com##sel, grouped by their sites
  std::map<std::pair<std::vector<std::string>, bool>, std::vector<std::string>> specific;
};

void network_filter(std::string line, Network &net, size_t &skipped) {
  bool exception = line.compare(0, 2, "@@") == 0;
  if (exception) line = line.substr(2);
  std::string pattern = line, options;
  size_t dollar = line.rfind('$');
  if (dollar != std::string::npos && !(line.size() > 1 && line[0] == '/' && line.back() == '/')) {
    pattern = line.substr(0, dollar);
    options = line.substr(dollar + 1);
  }
  if (pattern.size() >= 2 && pattern[0] == '/' && pattern.back() == '/') { skipped++; return; } // regex filter

  Trigger t;
  std::set<std::string> types, not_types;
  bool any_type = false, document = false, subdocument_only = true;
  for (const std::string &raw : split(options, ',')) {
    std::string o = lower(trim(raw));
    if (o.empty()) continue;
    bool neg = o[0] == '~';
    std::string name = neg ? o.substr(1) : o;
    if (name == "third-party" || name == "3p") {
      t.load_type = {neg ? "first-party" : "third-party"};
    } else if (name == "first-party" || name == "1p") {
      t.load_type = {neg ? "third-party" : "first-party"};
    } else if (name == "match-case") {
      t.case_sensitive = true;
    } else if (name.compare(0, 7, "domain=") == 0 && !neg) {
      if (!domains(o.substr(7), '|', t.if_domain, t.unless_domain)) { skipped++; return; }
    } else if (name == "important" || name == "all") {
    } else if (name == "document" || name == "doc") {
      document = true;
    } else if (type_names().count(name)) {
      const auto &wk = type_names().at(name);
      if (name != "subdocument" && name != "frame") subdocument_only = false;
      if (neg) {
        not_types.insert(wk.begin(), wk.end());
      } else {
        any_type = true;
        types.insert(wk.begin(), wk.end());
      }
    } else {
      // $redirect, $csp, $removeparam, $elemhide, $generichide, $badfilter, ...
      skipped++;
      return;
    }
  }

  if (exception && document) {
    // "@@||site^$document": nothing is blocked on that site
    std::string p = pattern;
    if (p.compare(0, 2, "||") == 0) p = p.substr(2);
    while (!p.empty() && (p.back() == '^' || p.back() == '/' || p.back() == '|')) p.pop_back();
    std::string d = abp_domain(p);
    if (d.empty() || pattern.compare(0, 2, "||") != 0) { skipped++; return; }
    Trigger a;
    a.if_domain = {d};
    net.allow_sites.push_back(rule(a, "ignore-previous-rules"));
    return;
  }

  t.url_filter = abp_url_filter(pattern);
  if (t.url_filter.empty()) { skipped++; return; }
  if (document) { types.insert("document"); any_type = true; }

  if (any_type) {
    if (types.empty()) { skipped++; return; } // only $object / $webrtc
    t.resource_types.assign(types.begin(), types.end());
    if (subdocument_only && !document && types.size() == 1 && types.count("document"))
      t.load_context = {"child-frame"};
  } else {
    // no type: what pages load. A listed host ("||adhost^") is blocked in
    // frames and as a page too (WebKit cannot tell frames apart in the same
    // rule); a pattern ("/ads/") never blocks the page you go to.
    for (const std::string &s : kSubresources)
      if (!not_types.count(s)) t.resource_types.push_back(s);
    if (pattern.compare(0, 2, "||") == 0 && !not_types.count("document")) t.resource_types.push_back("document");
    if (t.resource_types.empty()) { skipped++; return; }
  }
  (exception ? net.except : net.block).push_back(rule(t, exception ? "ignore-previous-rules" : "block"));
}

// "sites##selector" and its kin; false when the line is not element hiding
bool cosmetic_filter(const std::string &line, Cosmetic &cos, bool enabled, size_t &skipped) {
  size_t p = line.find('#');
  if (p == std::string::npos) return false;
  std::string sites = line.substr(0, p);
  for (char c : sites)
    if (!(isalnum((unsigned char)c) || c == '.' || c == ',' || c == '~' || c == '-' || c == '_' || c == '*'))
      return false; // a '#' in an address
  std::string rest = line.substr(p);
  bool exempt;
  std::string sel;
  if (rest.compare(0, 2, "##") == 0) { exempt = false; sel = rest.substr(2); }
  else if (rest.compare(0, 3, "#@#") == 0) { exempt = true; sel = rest.substr(3); }
  else if (rest.size() > 2 && rest[2] == '#' && (rest[1] == '?' || rest[1] == '$' || rest[1] == '%' || rest[1] == '@')) {
    skipped++; // #?# #$# #%# (procedural, CSS injection, scripts)
    return true;
  } else if (rest.size() > 3 && rest[1] == '@' && rest[3] == '#') {
    skipped++; // #@?# #@$# #@%#
    return true;
  } else {
    return false;
  }
  sel = trim(sel);
  if (!enabled) return true;
  if (!plain_selector(sel)) { skipped++; return true; }
  std::vector<std::string> yes, no;
  if (!domains(lower(sites), ',', yes, no)) { skipped++; return true; }
  if (exempt) {
    if (yes.empty()) cos.generic_exempt.insert(sel);
    else for (auto &d : yes) cos.generic_exempt_on[sel].push_back(d);
    return true;
  }
  if (yes.empty() && no.empty()) {
    if (cos.generic_seen.insert(sel).second) cos.generic.push_back(sel);
  } else if (!yes.empty()) {
    std::sort(yes.begin(), yes.end());
    cos.specific[{yes, true}].push_back(sel);
  } else {
    std::sort(no.begin(), no.end());
    cos.specific[{no, false}].push_back(sel);
  }
  return true;
}

std::string join_selectors(const std::vector<std::string> &v, size_t from, size_t to) {
  std::string s;
  for (size_t i = from; i < to; i++) {
    if (i > from) s += ", ";
    s += v[i];
  }
  return s;
}

} // namespace

std::string abp_domain(const std::string &domain) {
  std::string d = lower(trim(domain));
  if (d.empty() || d.front() == '.' || d.back() == '.' || d.front() == '-') return "";
  for (char c : d)
    if (!(isalnum((unsigned char)c) || c == '.' || c == '-' || c == '_')) return "";
  return "*" + d;
}

std::string abp_url_filter(const std::string &pattern) {
  std::string p = pattern, out;
  bool domain_anchor = false;
  if (p.compare(0, 2, "||") == 0) {
    out = kDomainStart;
    p = p.substr(2);
    domain_anchor = true;
  } else if (p.compare(0, 1, "|") == 0) {
    out = "^";
    p = p.substr(1);
  }
  bool end_anchor = !p.empty() && p.back() == '|';
  if (end_anchor) p.pop_back();
  if (!domain_anchor && out.empty())
    while (!p.empty() && p.front() == '*') p.erase(0, 1);
  if (!end_anchor)
    while (!p.empty() && p.back() == '*') p.pop_back();
  for (char c : p) {
    unsigned char u = (unsigned char)c;
    if (u >= 0x80 || u < 0x20) return ""; // WebKit takes ASCII only
    switch (c) {
    case '*': out += ".*"; break;
    case '^': out += kSeparator; break;
    case '|': return ""; // alternation is not supported
    case '.': case '+': case '?': case '(': case ')': case '[': case ']': case '{': case '}':
    case '\\': case '$':
      out += '\\';
      out += c;
      break;
    default: out += c;
    }
  }
  if (end_anchor) out += '$';
  if (out.empty()) out = ".*";
  return out;
}

AdblockResult abp_to_webkit(const std::vector<std::string> &lists, const std::vector<std::string> &allow,
                            bool cosmetic, size_t max_rules) {
  AdblockResult r;
  Network net;
  Cosmetic cos;
  for (const std::string &text : lists) {
    size_t start = 0;
    while (start < text.size()) {
      size_t nl = text.find('\n', start);
      if (nl == std::string::npos) nl = text.size();
      std::string line = trim(text.substr(start, nl - start));
      start = nl + 1;
      if (line.empty() || line[0] == '!' || line[0] == '[') continue;
      if (cosmetic_filter(line, cos, cosmetic, r.skipped)) continue;
      if (line.find("$$") != std::string::npos || line.find("$@$") != std::string::npos) { r.skipped++; continue; }
      network_filter(line, net, r.skipped);
    }
  }

  std::vector<std::string> hide;
  // generic element hiding, a batch of selectors per rule; selectors with
  // exceptions on some sites get a rule of their own
  std::vector<std::string> batch;
  for (const std::string &sel : cos.generic) {
    if (cos.generic_exempt.count(sel)) continue;
    auto ex = cos.generic_exempt_on.find(sel);
    if (ex != cos.generic_exempt_on.end()) {
      Trigger t;
      t.unless_domain = ex->second;
      hide.push_back(rule(t, "css-display-none", sel));
      continue;
    }
    batch.push_back(sel);
  }
  for (size_t i = 0; i < batch.size(); i += kSelectorsPerRule)
    hide.push_back(rule(Trigger{}, "css-display-none", join_selectors(batch, i, std::min(batch.size(), i + kSelectorsPerRule))));
  for (auto &kv : cos.specific) {
    std::vector<std::string> sels;
    for (auto &s : kv.second)
      if (!cos.generic_exempt.count(s)) sels.push_back(s);
    for (size_t i = 0; i < sels.size(); i += kSelectorsPerRule) {
      Trigger t;
      (kv.first.second ? t.if_domain : t.unless_domain) = kv.first.first;
      hide.push_back(rule(t, "css-display-none", join_selectors(sels, i, std::min(sels.size(), i + kSelectorsPerRule))));
    }
  }

  std::vector<std::string> allow_rules = net.allow_sites;
  for (const std::string &h : allow) {
    std::string d = abp_domain(h);
    if (d.empty()) continue;
    Trigger t;
    t.if_domain = {d};
    allow_rules.push_back(rule(t, "ignore-previous-rules"));
  }

  // order: blocking, its exceptions, element hiding (after the exceptions,
  // which would otherwise undo it), then whole-site exceptions
  size_t fixed = net.except.size() + allow_rules.size();
  size_t room = max_rules > fixed ? max_rules - fixed : 0;
  size_t nblock = std::min(net.block.size(), room);
  size_t nhide = std::min(hide.size(), room - nblock);
  r.skipped += (net.block.size() - nblock) + (hide.size() - nhide);
  std::string &out = r.json;
  out = "[";
  auto add = [&](const std::string &s) {
    if (r.rules) out += ",\n";
    out += s;
    r.rules++;
  };
  for (size_t i = 0; i < nblock; i++) add(net.block[i]);
  for (auto &s : net.except) add(s);
  for (size_t i = 0; i < nhide; i++) add(hide[i]);
  for (auto &s : allow_rules) add(s);
  out += "]";
  return r;
}

} // namespace web
