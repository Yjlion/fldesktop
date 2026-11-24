// fl-term: the lines scrolled off the top of the screen
//
// A ring of packed rows: the characters as UTF-8 with an offset per cell, and
// the attributes and colours as runs of cells that share them. A cell takes
// about 3 bytes instead of a VTermScreenCell's 40, and rows are reused once
// the ring is full, so scrolling allocates nothing. Colours are stored as
// libvterm gives them (indexed ones are converted when drawn, with the
// palette of that moment).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <vterm.h>

namespace term {

class Scrollback {
public:
  explicit Scrollback(size_t limit = 10000) : limit_(limit ? limit : 1) {}
  // the most lines kept; older ones are dropped
  void set_limit(size_t n);
  size_t limit() const { return limit_; }
  size_t size() const { return count_; }
  bool empty() const { return count_ == 0; }
  void clear();

  // Adds a line at the bottom (trailing blank cells are not stored). True
  // when the oldest line was dropped to make room.
  bool push(const VTermScreenCell *cells, int cols);
  // Takes the bottom line back into `cols` cells; the cells past its end get
  // the default colours. False when there is none.
  bool pop(VTermScreenCell *cells, int cols, const VTermColor &fg, const VTermColor &bg);
  // The cell `col` of line `line` (0 = the oldest); false past the line's
  // stored cells (a blank cell with the default colours is meant).
  bool get(size_t line, int col, VTermScreenCell &cell) const;
  // cells stored for a line
  int cells(size_t line) const;
  // bytes held (for tests)
  size_t memory() const;

private:
  struct Span {
    uint16_t start; // the first cell
    char width;
    VTermScreenCellAttrs attrs;
    VTermColor fg, bg;
  };
  struct Row {
    std::string text;           // UTF-8; 0xff marks the right half of a wide character
    std::vector<uint16_t> offs; // where each cell's text starts
    std::vector<Span> spans;    // by start
    uint16_t n = 0;             // cells
  };
  const Row &row(size_t line) const { return rows_[(head_ + line) % rows_.size()]; }
  void decode(const Row &r, int col, VTermScreenCell &cell) const;
  std::vector<Row> rows_;
  size_t head_ = 0, count_ = 0, limit_;
};

} // namespace term
