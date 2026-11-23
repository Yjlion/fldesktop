#include "settings_search.h"

#include <algorithm>
#include <cctype>

namespace settings_search {

const std::vector<Item> &index() {
  static const std::vector<Item> items = {
#include "settings_index.inc"
  };
  return items;
}

std::string normalize(const std::string &l) {
  std::string out;
  for (size_t i = 0; i < l.size(); i++) {
    char c = l[i];
    if (c == '&' && i + 1 < l.size() && std::isalnum((unsigned char)l[i + 1])) continue;
    if (c == '@') { // an FLTK symbol up to the next space
      while (i < l.size() && l[i] != ' ') i++;
      continue;
    }
    out += (char)std::tolower((unsigned char)c);
  }
  while (!out.empty() && (out.back() == ':' || out.back() == ' ' || out.back() == '.')) out.pop_back();
  if (out.size() >= 3 && out.compare(out.size() - 3, 3, "\xe2\x80\xa6") == 0) out.resize(out.size() - 3);
  size_t s = out.find_first_not_of(' ');
  return s == std::string::npos ? "" : out.substr(s);
}

static std::vector<std::string> words(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80) cur += (char)std::tolower((unsigned char)c);
    else if (!cur.empty()) {
      out.push_back(cur);
      cur.clear();
    }
  }
  if (!cur.empty()) out.push_back(cur);
  return out;
}

std::vector<Hit> search(const std::string &query, const std::vector<Item> &items, size_t max) {
  std::vector<Hit> out;
  auto q = words(query);
  if (q.empty()) return out;
  std::string ql = normalize(query);
  for (auto &it : items) {
    std::string l = normalize(it.label);
    auto lw = words(l);
    int score = 0;
    bool all = true;
    for (auto &w : q) {
      bool prefix = false;
      for (auto &x : lw)
        if (x.compare(0, w.size(), w) == 0) prefix = true;
      if (prefix) score += 2;
      else if (w.size() >= 4 && l.find(w) != std::string::npos) score += 1;
      else {
        all = false;
        break;
      }
    }
    if (!all) continue;
    if (l.compare(0, ql.size(), ql) == 0) score += 3; // the label starts with the query
    if (l == ql) score += 3;
    out.push_back({it.page, it.label, score});
  }
  std::stable_sort(out.begin(), out.end(), [](const Hit &a, const Hit &b) {
    if (a.score != b.score) return a.score > b.score;
    return a.label.size() < b.label.size();
  });
  if (out.size() > max) out.resize(max);
  return out;
}

} // namespace settings_search
