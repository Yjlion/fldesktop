#include "rename_util.h"

#include <cctype>
#include <map>
#include <regex>
#include <set>

namespace frename {

std::pair<std::string, std::string> split_ext(const std::string &name) {
  size_t dot = name.rfind('.');
  if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return {name, ""};
  return {name.substr(0, dot), name.substr(dot)};
}

static std::string counter(int n, int width) {
  std::string s = std::to_string(n < 0 ? -n : n);
  while ((int)s.size() < width) s.insert(s.begin(), '0');
  return n < 0 ? "-" + s : s;
}

static std::string expand(const std::string &pat, const std::string &stem, const std::string &ext, int n) {
  std::string out;
  for (size_t i = 0; i < pat.size(); i++) {
    if (pat[i] == '{') {
      size_t close = pat.find('}', i);
      if (close != std::string::npos) {
        std::string tok = pat.substr(i + 1, close - i - 1);
        bool done = true;
        if (tok == "name") out += stem;
        else if (tok == "ext") out += ext;
        else if (tok == "n") out += counter(n, 0);
        else if (tok.size() > 2 && tok.compare(0, 2, "n:") == 0 &&
                 tok.find_first_not_of("0123456789", 2) == std::string::npos)
          out += counter(n, std::min(12, std::stoi(tok.substr(2))));
        else done = false;
        if (done) {
          i = close;
          continue;
        }
      }
    }
    out += pat[i];
  }
  return out;
}

static std::string replace_plain(const std::string &s, const std::string &find, const std::string &repl, bool icase) {
  if (find.empty()) return s;
  auto lower = [](std::string t) {
    for (auto &c : t) c = (char)std::tolower((unsigned char)c);
    return t;
  };
  std::string hay = icase ? lower(s) : s, needle = icase ? lower(find) : find, out;
  size_t pos = 0;
  for (;;) {
    size_t k = hay.find(needle, pos);
    if (k == std::string::npos) break;
    out += s.substr(pos, k - pos) + repl;
    pos = k + find.size();
  }
  return out + s.substr(pos);
}

static std::string apply_case(const std::string &s, Case c) {
  std::string out = s;
  bool start = true;
  for (auto &ch : out) {
    unsigned char u = (unsigned char)ch;
    if (u >= 0x80) { start = false; continue; } // leave UTF-8 alone
    switch (c) {
      case Case::Keep: break;
      case Case::Lower: ch = (char)std::tolower(u); break;
      case Case::Upper: ch = (char)std::toupper(u); break;
      case Case::Title:
        ch = (char)(start ? std::toupper(u) : std::tolower(u));
        break;
    }
    start = !std::isalnum(u) && u != '\'';
  }
  return out;
}

std::vector<std::string> apply(const std::vector<std::string> &names, const Rule &r, std::string *err) {
  std::vector<std::string> out;
  std::regex re;
  if (r.regex && !r.find.empty()) {
    try {
      re = std::regex(r.find, r.ignore_case ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript);
    } catch (const std::regex_error &e) {
      if (err) *err = std::string("Bad regular expression: ") + e.what();
      return {};
    }
  }
  int n = r.start;
  for (auto &name : names) {
    auto se = split_ext(name);
    std::string s = expand(r.pattern.empty() ? "{name}{ext}" : r.pattern, se.first, se.second, n);
    n += r.step;
    if (!r.find.empty()) s = r.regex ? std::regex_replace(s, re, r.replace) : replace_plain(s, r.find, r.replace, r.ignore_case);
    if (r.letter_case != Case::Keep) {
      auto ne = split_ext(s);
      s = apply_case(ne.first, r.letter_case) + ne.second;
    }
    out.push_back(s);
  }
  if (err) err->clear();
  return out;
}

std::vector<std::string> problems(const std::vector<std::string> &old_names, const std::vector<std::string> &new_names) {
  std::vector<std::string> out(new_names.size());
  std::map<std::string, int> count;
  for (auto &n : new_names) count[n]++;
  for (size_t i = 0; i < new_names.size(); i++) {
    const std::string &n = new_names[i];
    if (n.empty()) out[i] = "empty name";
    else if (n.find('/') != std::string::npos) out[i] = "contains /";
    else if (n == "." || n == "..") out[i] = "not a file name";
    else if (n.size() > 255) out[i] = "too long";
    else if (count[n] > 1) out[i] = "used twice";
  }
  (void)old_names;
  return out;
}

std::vector<std::pair<std::string, std::string>> plan(const std::vector<std::string> &old_names,
                                                      const std::vector<std::string> &new_names,
                                                      const std::string &temp_prefix) {
  std::vector<std::pair<std::string, std::string>> out, second;
  std::set<std::string> olds;
  for (size_t i = 0; i < old_names.size() && i < new_names.size(); i++)
    if (old_names[i] != new_names[i]) olds.insert(old_names[i]);
  for (size_t i = 0; i < old_names.size() && i < new_names.size(); i++) {
    const std::string &a = old_names[i], &b = new_names[i];
    if (a == b) continue;
    if (olds.count(b)) {
      // b is still taken by a file that moves away later: go through a temporary name
      std::string t = temp_prefix + std::to_string(i);
      out.push_back({a, t});
      second.push_back({t, b});
    } else {
      out.push_back({a, b});
    }
  }
  out.insert(out.end(), second.begin(), second.end());
  return out;
}

} // namespace frename
