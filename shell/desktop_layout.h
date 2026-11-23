// Where fl-desktop puts its icons: a grid of cells, column by column. Icons
// the user moved keep their cell (desktop-icons.ini); the others fill the
// free cells in their usual order.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace desk {

using Cell = std::pair<int, int>; // column, row

// The cells of `keys` (in order) on a cols x rows grid. A saved cell is kept
// when it is on the grid and no earlier key has it; the other keys take the
// free cells column by column (past the last column when the grid is full).
std::vector<Cell> place(const std::vector<std::string> &keys, const std::map<std::string, Cell> &saved, int cols,
                        int rows);

// Moves `moving` (some of `keys`, whose cells are `cells`) so that `anchor`
// lands on `target` and the others keep their places around it. Cells off
// the grid or taken by an icon that stays are replaced by the next free cell
// (column by column). Returns the cell of every key: what to save.
std::map<std::string, Cell> move(const std::vector<std::string> &keys, const std::vector<Cell> &cells,
                                 const std::vector<std::string> &moving, const std::string &anchor, Cell target,
                                 int cols, int rows);

// "3,1" <-> {3, 1}
std::string format_cell(Cell c);
bool parse_cell(const std::string &s, Cell &c);

} // namespace desk
