#include "term_runs.h"

namespace term {

void split_runs(const std::vector<RunCell> &cells, std::vector<Run> &out) {
  out.clear();
  int open = -1, last = -1;
  uint32_t key = 0;
  auto close = [&] {
    if (open >= 0) out.push_back({open, last, false});
    open = -1;
  };
  for (int i = 0; i < (int)cells.size(); i++) {
    const RunCell &c = cells[i];
    if (c.ch == 0 || c.ch == ' ') {
      // a blank cell wider than one column would shift the text after it
      if (c.width != 1) close();
      continue;
    }
    bool plain = !c.alone && c.width == 1 && c.ch > ' ' && c.ch < 0x7f;
    if (plain && open >= 0 && c.key == key) {
      last = i;
      continue;
    }
    close();
    if (plain) {
      open = last = i;
      key = c.key;
    } else {
      out.push_back({i, i, true});
    }
  }
  close();
}

std::vector<Run> split_runs(const std::vector<RunCell> &cells) {
  std::vector<Run> r;
  split_runs(cells, r);
  return r;
}

} // namespace term
