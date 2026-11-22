// fl-docs: flowing text laid out a step at a time gives the same pages
#include "flow.h"
#include "test.h"

using namespace docs;
using namespace docs::flow;

int main() {
  std::vector<Block> blocks;
  for (int i = 0; i < 400; i++) {
    Block b;
    b.markup = "Paragraph " + std::to_string(i) + ": ";
    for (int k = 0; k < i % 17 + 1; k++) b.markup += "the quick brown fox jumps over the lazy dog ";
    if (i % 50 == 49) b.markup += "MARKER";
    blocks.push_back(b);
    if (i % 120 == 119) {
      Block br;
      br.kind = Block::PageBreak;
      blocks.push_back(br);
    }
  }
  PageStyle style;

  Document whole;
  whole.layout(blocks, style);
  CHECK(whole.done());
  size_t n = whole.sizes().size();
  CHECK(n > 20);

  // a block per step: the pages so far are there and only grow
  Document steps;
  steps.begin(blocks, style);
  CHECK(!steps.done());
  size_t shown = 0, calls = 0;
  bool grew = true;
  while (!steps.step(0)) {
    size_t now = steps.sizes().size();
    if (now < shown) grew = false;
    shown = now;
    calls++;
  }
  CHECK(grew);
  CHECK(calls > 100);
  CHECK(steps.done());
  CHECK_EQ(steps.sizes().size(), n);
  std::vector<Hit> a = whole.find("MARKER"), b = steps.find("MARKER");
  CHECK_EQ(a.size(), (size_t)8);
  CHECK_EQ(b.size(), a.size());
  for (size_t i = 0; i < a.size() && i < b.size(); i++) {
    CHECK_EQ(a[i].page, b[i].page);
    CHECK_EQ(a[i].y, b[i].y);
  }
  CHECK_EQ(a.back().page + 1, (int)n);
  // nothing to lay out
  Document empty;
  empty.begin({}, style);
  CHECK(empty.step(0.01));
  CHECK_EQ(empty.sizes().size(), (size_t)1);
  return test_result("flow");
}
