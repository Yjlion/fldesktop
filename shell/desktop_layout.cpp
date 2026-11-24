#include "desktop_layout.h"

#include <algorithm>
#include <cstdio>
#include <set>

namespace desk {

namespace {

bool on_grid(Cell c, int cols, int rows) {
  return c.first >= 0 && c.first < cols && c.second >= 0 && c.second < rows;
}

// the first free cell at or after `from`, column by column, going round to
// the first column; past the last column when the grid is full
// noipa: GCC 16.2 with -O2 -flto (makepkg's default) lets IPA-CP use a copy
// specialized for cols == 1 (from a place() call) for every caller
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noipa))
#endif
Cell next_free(const std::set<Cell> &taken, Cell from, int cols, int rows) {
  const int n = cols * rows;
  const int start = (from.first < 0 ? 0 : from.first) * rows + (from.second < 0 ? 0 : from.second);
  if (start < n) {
    for (int k = 0; k < n; k++) {
      const int i = (start + k) % n;
      const Cell c(i / rows, i % rows);
      if (taken.find(c) == taken.end()) return c;
    }
  }
  for (int i = start > n ? start : n;; i++) {
    const Cell c(i / rows, i % rows);
    if (taken.find(c) == taken.end()) return c;
  }
}

} // namespace

std::vector<Cell> place(const std::vector<std::string> &keys, const std::map<std::string, Cell> &saved, int cols,
                        int rows) {
  rows = std::max(1, rows);
  cols = std::max(1, cols);
  std::vector<Cell> out(keys.size(), {-1, -1});
  std::vector<bool> done(keys.size(), false);
  std::set<Cell> taken;
  for (size_t i = 0; i < keys.size(); i++) {
    auto s = saved.find(keys[i]);
    if (s == saved.end() || !on_grid(s->second, cols, rows) || taken.count(s->second)) continue;
    out[i] = s->second;
    done[i] = true;
    taken.insert(s->second);
  }
  Cell next{0, 0};
  for (size_t i = 0; i < keys.size(); i++) {
    if (done[i]) continue;
    out[i] = next_free(taken, next, cols, rows);
    taken.insert(out[i]);
    next = out[i];
  }
  return out;
}

std::map<std::string, Cell> move(const std::vector<std::string> &keys, const std::vector<Cell> &cells,
                                 const std::vector<std::string> &moving, const std::string &anchor, Cell target,
                                 int cols, int rows) {
  rows = std::max(1, rows);
  cols = std::max(1, cols);
  std::map<std::string, Cell> out;
  std::set<std::string> mv(moving.begin(), moving.end());
  Cell from{0, 0};
  for (size_t i = 0; i < keys.size() && i < cells.size(); i++)
    if (keys[i] == anchor) from = cells[i];
  // the icons that stay keep their cells
  std::set<Cell> taken;
  for (size_t i = 0; i < keys.size() && i < cells.size(); i++)
    if (!mv.count(keys[i])) {
      out[keys[i]] = cells[i];
      taken.insert(cells[i]);
    }
  // the anchor first, then the others in their order
  std::vector<size_t> order;
  for (size_t i = 0; i < keys.size() && i < cells.size(); i++)
    if (mv.count(keys[i]) && keys[i] == anchor) order.insert(order.begin(), i);
    else if (mv.count(keys[i])) order.push_back(i);
  for (size_t i : order) {
    Cell c{cells[i].first - from.first + target.first, cells[i].second - from.second + target.second};
    c.first = std::min(std::max(c.first, 0), cols - 1);
    c.second = std::min(std::max(c.second, 0), rows - 1);
    if (taken.count(c)) c = next_free(taken, c, cols, rows);
    out[keys[i]] = c;
    taken.insert(c);
  }
  return out;
}

std::string format_cell(Cell c) { return std::to_string(c.first) + "," + std::to_string(c.second); }

bool parse_cell(const std::string &s, Cell &c) {
  int a, b;
  char extra;
  if (sscanf(s.c_str(), "%d,%d%c", &a, &b, &extra) != 2 || a < 0 || b < 0) return false;
  c = {a, b};
  return true;
}

} // namespace desk
