// fl-term: split panes. A tab's panes form a tree: each split holds two
// parts side by side or one above the other, with a ratio. No FLTK here.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace term {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

enum class Dir { Left, Right, Up, Down };

class PaneTree {
public:
  using Id = intptr_t;
  explicit PaneTree(Id first = 0);
  ~PaneTree();
  PaneTree(PaneTree &&) noexcept;
  PaneTree &operator=(PaneTree &&) noexcept;

  void reset(Id first);
  // id's place is shared with added (to its right, or below)
  bool split(Id id, Id added, bool side_by_side);
  // the pane goes; its sibling takes the split's place. False for the last pane
  bool remove(Id id);
  bool contains(Id id) const;
  size_t count() const;
  std::vector<Id> ids() const; // left to right, top to bottom
  // the rectangle of every pane; gap pixels between parts
  std::vector<std::pair<Id, Rect>> layout(const Rect &r, int gap) const;
  // the gaps between parts
  std::vector<Rect> dividers(const Rect &r, int gap) const;
  // the pane next to id in a direction (nearest by center), or 0
  Id neighbor(Id id, Dir d, const Rect &r, int gap) const;
  // dragging a divider: the split under (x, y) in the gap; begin returns false when none
  bool drag_begin(const Rect &r, int gap, int x, int y);
  void drag_to(int x, int y);
  void drag_end();
  bool dragging() const { return drag_ != nullptr; }
  // whether a divider under (x, y) is between parts side by side (a vertical bar)
  bool divider_at(const Rect &r, int gap, int x, int y, bool *vertical_bar) const;
  // "a|b" style text of the tree, for tests: (h a b) side by side, (v a b) above and below
  std::string describe() const;

  struct Node;

private:
  std::unique_ptr<Node> root_;
  Node *drag_ = nullptr;
  Rect drag_rect_;
};

// ---- scrollback search -------------------------------------------------------------

struct Match {
  long line = -1;
  int col = 0, len = 0;
};

// lines: one char32 per column. Finds needle starting after (from_line,
// from_col) going to newer lines, or before it going to older lines
// (backwards), wrapping around. icase: ASCII and Latin case folding.
Match find_in_lines(const std::vector<std::u32string> &lines, const std::u32string &needle, long from_line,
                    int from_col, bool backwards, bool icase);
std::u32string to_u32(const std::string &utf8);

} // namespace term
