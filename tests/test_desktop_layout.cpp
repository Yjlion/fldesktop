// fl-desktop's icon grid: saved cells, filling, moving icons
#include "../shell/desktop_layout.h"
#include "test.h"

using desk::Cell;

int main() {
  // nothing saved: column by column
  {
    auto c = desk::place({"a", "b", "c", "d", "e"}, {}, 3, 2);
    CHECK(c[0] == Cell(0, 0));
    CHECK(c[1] == Cell(0, 1));
    CHECK(c[2] == Cell(1, 0));
    CHECK(c[4] == Cell(2, 0));
  }
  // a saved cell is kept; the others skip it
  {
    auto c = desk::place({"a", "b", "c"}, {{"c", {0, 1}}}, 3, 2);
    CHECK(c[0] == Cell(0, 0));
    CHECK(c[1] == Cell(1, 0));
    CHECK(c[2] == Cell(0, 1));
  }
  // off the grid (a smaller screen) or taken: placed like the others
  {
    auto c = desk::place({"a", "b", "c"}, {{"a", {9, 9}}, {"b", {1, 1}}, {"c", {1, 1}}}, 3, 2);
    CHECK(c[0] == Cell(0, 0));
    CHECK(c[1] == Cell(1, 1));
    CHECK(c[2] == Cell(0, 1));
  }
  // a full grid goes on past the last column
  {
    auto c = desk::place({"a", "b", "c"}, {}, 1, 2);
    CHECK(c[2] == Cell(1, 0));
  }
  // moving one icon to a free cell; the others stay
  {
    std::vector<std::string> k{"a", "b", "c"};
    auto cells = desk::place(k, {}, 4, 3);
    auto m = desk::move(k, cells, {"c"}, "c", {3, 2}, 4, 3);
    CHECK(m["c"] == Cell(3, 2));
    CHECK(m["a"] == Cell(0, 0));
    CHECK(m["b"] == Cell(0, 1));
  }
  // a group keeps its shape around the icon dragged
  {
    std::vector<std::string> k{"a", "b", "c"};
    auto cells = desk::place(k, {}, 4, 3); // a 0,0  b 0,1  c 0,2
    auto m = desk::move(k, cells, {"b", "c"}, "b", {2, 0}, 4, 3);
    CHECK(m["b"] == Cell(2, 0));
    CHECK(m["c"] == Cell(2, 1));
  }
  // onto an icon that stays: the next free cell; off the edge: clamped
  {
    std::vector<std::string> k{"a", "b", "c"};
    auto cells = desk::place(k, {}, 4, 3);
    auto m = desk::move(k, cells, {"c"}, "c", {0, 0}, 4, 3);
    CHECK(m["c"] == Cell(0, 2));
    m = desk::move(k, cells, {"b", "c"}, "b", {3, 2}, 4, 3);
    CHECK(m["b"] == Cell(3, 2));
    CHECK(m["c"] == Cell(0, 1)); // 3,3 is off the grid and 3,2 taken: round to the first free cell
  }
  {
    Cell c;
    CHECK(desk::parse_cell("4,1", c) && c == Cell(4, 1));
    CHECK(!desk::parse_cell("4", c));
    CHECK(!desk::parse_cell("-1,2", c));
    CHECK(!desk::parse_cell("1,2x", c));
    CHECK_EQ(desk::format_cell({4, 1}), std::string("4,1"));
  }
  return test_result("desktop_layout");
}
