// fl-term's packed scrollback: lines come back as they went in
#include <cstring>
#include <vector>

#include "scrollback.h"
#include "test.h"

using term::Scrollback;

static VTermScreenCell cell(uint32_t ch, int fg_idx = -1, bool bold = false, int width = 1) {
  VTermScreenCell c;
  memset(&c, 0, sizeof c);
  c.chars[0] = ch;
  c.width = (char)width;
  c.attrs.bold = bold;
  if (fg_idx < 0) {
    vterm_color_rgb(&c.fg, 200, 200, 200);
    c.fg.type |= VTERM_COLOR_DEFAULT_FG;
  } else {
    vterm_color_indexed(&c.fg, (uint8_t)fg_idx);
  }
  vterm_color_rgb(&c.bg, 0, 0, 0);
  c.bg.type |= VTERM_COLOR_DEFAULT_BG;
  return c;
}

static std::vector<VTermScreenCell> line(const char32_t *text, int cols) {
  std::vector<VTermScreenCell> v;
  bool end = false;
  for (int i = 0; i < cols; i++) {
    if (!end && !text[i]) end = true; // not past the terminator
    v.push_back(cell(end ? 0 : text[i]));
  }
  return v;
}

static bool same(const VTermScreenCell &a, const VTermScreenCell &b) {
  return memcmp(a.chars, b.chars, sizeof a.chars) == 0 && a.width == b.width &&
         memcmp(&a.attrs, &b.attrs, sizeof a.attrs) == 0 && memcmp(&a.fg, &b.fg, sizeof a.fg) == 0 &&
         memcmp(&a.bg, &b.bg, sizeof a.bg) == 0;
}

int main() {
  Scrollback sb(3);
  CHECK(sb.empty());
  // text, colours, attributes, a wide character and a combining one
  std::vector<VTermScreenCell> l1 = {cell('a'), cell(U'é', 1), cell(U'中', 2, true, 2), cell((uint32_t)-1, 2, true, 2),
                                     cell('x'), cell(U'\U0001F600')};
  l1[4].chars[1] = 0x301; // x + combining acute
  CHECK(!sb.push(l1.data(), (int)l1.size()));
  CHECK_EQ(sb.size(), (size_t)1);
  CHECK_EQ(sb.cells(0), 6);
  for (int i = 0; i < 6; i++) {
    VTermScreenCell c;
    CHECK(sb.get(0, i, c));
    CHECK(same(c, l1[i]));
  }
  // trailing blanks are not stored
  auto l2 = line(U"hi", 80);
  sb.push(l2.data(), 80);
  CHECK_EQ(sb.cells(1), 2);
  VTermScreenCell c;
  CHECK(!sb.get(1, 5, c));
  // the ring keeps the newest three
  auto l3 = line(U"three", 10), l4 = line(U"four", 10);
  CHECK(!sb.push(l3.data(), 10));
  CHECK(sb.push(l4.data(), 10)); // drops l1
  CHECK_EQ(sb.size(), (size_t)3);
  CHECK(sb.get(0, 1, c) && c.chars[0] == 'i');
  CHECK(sb.get(2, 0, c) && c.chars[0] == 'f');
  // popping gives the bottom line back, padded with the default colours
  VTermColor fg, bg;
  vterm_color_rgb(&fg, 1, 2, 3);
  vterm_color_rgb(&bg, 4, 5, 6);
  std::vector<VTermScreenCell> out(10);
  CHECK(sb.pop(out.data(), 10, fg, bg));
  CHECK(out[0].chars[0] == 'f' && out[3].chars[0] == 'r');
  CHECK(out[6].chars[0] == 0 && memcmp(&out[6].bg, &bg, sizeof bg) == 0 && out[6].width == 1);
  CHECK_EQ(sb.size(), (size_t)2);
  // after a pop the next push reuses the slot
  CHECK(!sb.push(l4.data(), 10));
  CHECK(sb.push(l3.data(), 10));
  CHECK(sb.get(2, 0, c) && c.chars[0] == 't');
  // a smaller limit keeps the newest
  sb.set_limit(1);
  CHECK_EQ(sb.size(), (size_t)1);
  CHECK(sb.get(0, 0, c) && c.chars[0] == 't');
  sb.clear();
  CHECK(sb.empty());
  CHECK(!sb.pop(out.data(), 10, fg, bg));
  // much smaller than a cell array: 10000 lines of 80 cells
  Scrollback big(10000);
  auto l5 = line(U"0123456789012345678901234567890123456789012345678901234567890123456789012345678", 80);
  for (int i = 0; i < 12000; i++) big.push(l5.data(), 80);
  CHECK_EQ(big.size(), (size_t)10000);
  CHECK(big.memory() < (size_t)10000 * 80 * sizeof(VTermScreenCell) / 6);
  return test_result("scrollback");
}
