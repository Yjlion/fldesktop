// fl-settings: searching the labels of all pages (settings_index.inc, made
// by tools/settings-index.py). No FLTK here (unit tested).
#pragma once
#include <string>
#include <vector>

namespace settings_search {

struct Item {
  const char *page, *label;
};
// the generated table; its labels are the source texts (msgids), as in the
// program: "Speed:", "&Tap to click"; show them through clean_label(_(label))
const std::vector<Item> &index();
// a label as the search shows it: without "&" markers, "@" symbols, line
// breaks and a trailing ":", "…" or "."
std::string clean_label(const std::string &label);

struct Hit {
  std::string page, label;
  int score = 0;
};

// items whose label has every word of the query (at the start of one of its
// words, or anywhere for longer words); best first
std::vector<Hit> search(const std::string &query, const std::vector<Item> &items, size_t max = 12);
// the text a widget label is compared by: lower case, without "&" markers,
// "@" symbols, a trailing ":" or "…"
std::string normalize(const std::string &label);

} // namespace settings_search
