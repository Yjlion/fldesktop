#include "scrollback.h"

#include <algorithm>
#include <cstring>

namespace term {

namespace {

void put_utf8(std::string &s, uint32_t c) {
  if (c < 0x80) {
    s += (char)c;
  } else if (c < 0x800) {
    s += (char)(0xc0 | (c >> 6));
    s += (char)(0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    s += (char)(0xe0 | (c >> 12));
    s += (char)(0x80 | ((c >> 6) & 0x3f));
    s += (char)(0x80 | (c & 0x3f));
  } else {
    s += (char)(0xf0 | ((c >> 18) & 0x07));
    s += (char)(0x80 | ((c >> 12) & 0x3f));
    s += (char)(0x80 | ((c >> 6) & 0x3f));
    s += (char)(0x80 | (c & 0x3f));
  }
}

// decodes one character at s[i], moving i past it
uint32_t get_utf8(const std::string &s, size_t &i) {
  unsigned char c = (unsigned char)s[i++];
  if (c < 0x80) return c;
  int n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
  uint32_t v = c & (0x3f >> n);
  while (n-- > 0 && i < s.size()) v = (v << 6) | ((unsigned char)s[i++] & 0x3f);
  return v;
}

bool same_look(const VTermScreenCell &c, const char width, const VTermScreenCellAttrs &a, const VTermColor &fg,
               const VTermColor &bg) {
  return c.width == width && memcmp(&c.attrs, &a, sizeof a) == 0 && memcmp(&c.fg, &fg, sizeof fg) == 0 &&
         memcmp(&c.bg, &bg, sizeof bg) == 0;
}

} // namespace

void Scrollback::set_limit(size_t n) {
  if (!n) n = 1;
  if (n == limit_) return;
  // keep the newest lines, oldest first
  std::vector<Row> keep;
  size_t from = count_ > n ? count_ - n : 0;
  keep.reserve(count_ - from);
  for (size_t i = from; i < count_; i++) keep.push_back(std::move(rows_[(head_ + i) % rows_.size()]));
  rows_ = std::move(keep);
  head_ = 0;
  count_ = rows_.size();
  limit_ = n;
}

void Scrollback::clear() {
  rows_.clear();
  rows_.shrink_to_fit();
  head_ = count_ = 0;
}

bool Scrollback::push(const VTermScreenCell *cells, int cols) {
  // trailing blank cells are not kept
  while (cols > 0 && cells[cols - 1].chars[0] == 0 && VTERM_COLOR_IS_DEFAULT_BG(&cells[cols - 1].bg)) cols--;
  if (cols > 0xfffe) cols = 0xfffe;
  bool dropped = false;
  Row *r;
  if (count_ < limit_) {
    if (rows_.size() < limit_ && count_ == rows_.size()) {
      rows_.emplace_back();
      r = &rows_.back();
    } else {
      r = &rows_[(head_ + count_) % rows_.size()];
    }
    count_++;
  } else {
    // full: the oldest row is reused for the new line
    r = &rows_[head_];
    head_ = (head_ + 1) % rows_.size();
    dropped = true;
  }
  r->text.clear();
  r->offs.clear();
  r->spans.clear();
  r->n = (uint16_t)cols;
  r->offs.reserve(cols);
  r->text.reserve(cols);
  for (int i = 0; i < cols; i++) {
    const VTermScreenCell &c = cells[i];
    r->offs.push_back((uint16_t)std::min<size_t>(r->text.size(), 0xffff));
    if (c.chars[0] == (uint32_t)-1) {
      r->text += '\xff';
    } else {
      for (int k = 0; k < VTERM_MAX_CHARS_PER_CELL && c.chars[k]; k++) put_utf8(r->text, c.chars[k]);
    }
    if (r->spans.empty() || !same_look(c, r->spans.back().width, r->spans.back().attrs, r->spans.back().fg,
                                       r->spans.back().bg))
      r->spans.push_back({(uint16_t)i, c.width, c.attrs, c.fg, c.bg});
  }
  return dropped;
}

void Scrollback::decode(const Row &r, int col, VTermScreenCell &cell) const {
  memset(&cell, 0, sizeof cell);
  size_t a = r.offs[col], b = col + 1 < (int)r.n ? r.offs[col + 1] : r.text.size();
  if (b > a && (unsigned char)r.text[a] == 0xff) {
    cell.chars[0] = (uint32_t)-1;
  } else {
    int k = 0;
    for (size_t i = a; i < b && k < VTERM_MAX_CHARS_PER_CELL;) cell.chars[k++] = get_utf8(r.text, i);
  }
  // the run the cell is in
  auto it = std::upper_bound(r.spans.begin(), r.spans.end(), col,
                             [](int c, const Span &s) { return c < (int)s.start; });
  const Span &s = *(it - 1);
  cell.width = s.width;
  cell.attrs = s.attrs;
  cell.fg = s.fg;
  cell.bg = s.bg;
}

bool Scrollback::get(size_t line, int col, VTermScreenCell &cell) const {
  if (line >= count_ || col < 0) return false;
  const Row &r = row(line);
  if (col >= (int)r.n) return false;
  decode(r, col, cell);
  return true;
}

int Scrollback::cells(size_t line) const { return line < count_ ? row(line).n : 0; }

bool Scrollback::pop(VTermScreenCell *cells, int cols, const VTermColor &fg, const VTermColor &bg) {
  if (!count_) return false;
  const Row &r = row(count_ - 1);
  for (int i = 0; i < cols; i++) {
    if (i < (int)r.n) {
      decode(r, i, cells[i]);
    } else {
      memset(&cells[i], 0, sizeof cells[i]);
      cells[i].width = 1;
      cells[i].fg = fg;
      cells[i].bg = bg;
    }
  }
  count_--;
  return true;
}

size_t Scrollback::memory() const {
  size_t m = rows_.capacity() * sizeof(Row);
  for (const Row &r : rows_) m += r.text.capacity() + r.offs.capacity() * 2 + r.spans.capacity() * sizeof(Span);
  return m;
}

} // namespace term
