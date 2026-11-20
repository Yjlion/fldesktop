// fl-term: splitting a screen row into pieces that can be drawn with one
// text call each. Drawing a run of cells as one string is far cheaper than
// one call per cell (each call lays the text out again).
#pragma once
#include <cstdint>
#include <vector>

namespace term {

struct RunCell {
  uint32_t ch = 0;     // first code point, 0 = empty
  int width = 1;       // columns the cell takes
  uint32_t key = 0;    // font style and colour: cells in a run share it
  bool alone = false;  // must be drawn by itself (cursor, combining characters)
};

struct Run {
  int first = 0, last = 0; // indexes into the cells, inclusive
  bool single = false;     // one cell drawn on its own
};

// Runs of printable ASCII cells (width 1) with the same key. Blank cells draw
// nothing and join the run around them; runs never start or end with one.
// Other non-blank cells come out as single runs. Blank cells outside runs are
// left out.
std::vector<Run> split_runs(const std::vector<RunCell> &cells);
void split_runs(const std::vector<RunCell> &cells, std::vector<Run> &out);

} // namespace term
