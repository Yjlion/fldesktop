// fl-docs: layout, zoom, coordinates and print ranges
#include "docs_util.h"
#include "test.h"

using namespace docs;

int main() {
  std::vector<PageSize> pages = {{612, 792}, {612, 792}, {792, 612}}; // letter, letter, landscape
  Layout l = layout(pages, 1.0);
  CHECK_EQ(l.w[0], 816); // 8.5 in at 96 dpi
  CHECK_EQ(l.h[0], 1056);
  CHECK_EQ(l.y[0], kGap);
  CHECK_EQ(l.y[1], kGap + 1056 + kGap);
  CHECK_EQ(l.width, 1056 + 2 * kGap); // the landscape page is the widest
  CHECK_EQ(l.x[0], (l.width - 816) / 2);
  CHECK_EQ(l.height, kGap + 1056 + kGap + 1056 + kGap + 816 + kGap);
  Layout wide = layout(pages, 1.0, 2000);
  CHECK_EQ(wide.width, 2000);
  CHECK_EQ(wide.x[2], (2000 - 1056) / 2);

  CHECK_EQ(page_at(l, 0), 0);
  CHECK_EQ(page_at(l, 500), 0);
  CHECK_EQ(page_at(l, l.y[1] + 10), 1);
  CHECK_EQ(page_at(l, 1000000), 2);
  CHECK_EQ(page_at(Layout{}, 10), -1);
  CHECK_EQ(current_page(l, l.y[1] - 100, 800), 1); // most of the view shows page 2
  CHECK_EQ(current_page(l, 0, 600), 0);

  double zw = fit_zoom(pages, 0, Fit::Width, 840, 600);
  CHECK(std::fabs(zw - 816.0 / 1056.0) < 0.01); // the landscape page is the widest
  CHECK(std::fabs(fit_zoom({{612, 792}}, 0, Fit::Width, 840, 600) - 1.0) < 0.01);
  double zp = fit_zoom(pages, 0, Fit::Page, 840, 600);
  CHECK(zp < zw);
  CHECK_EQ(fit_zoom(pages, 0, Fit::None, 840, 600), 1.0);
  CHECK_EQ(zoom_step(1.0, 1), 1.1);
  CHECK_EQ(zoom_step(1.0, -1), 0.9);
  CHECK_EQ(zoom_step(1.05, 1), 1.1);
  CHECK_EQ(zoom_step(8.0, 1), 8.0);
  CHECK_EQ(zoom_step(0.1, -1), 0.1);

  Rect r = to_strip(l, 1, 72, 72, 72, 18); // one inch in, one inch wide
  CHECK_EQ(r.x, l.x[1] + 96);
  CHECK_EQ(r.y, l.y[1] + 96);
  CHECK_EQ(r.w, 96);
  CHECK_EQ(r.h, 24);
  CHECK_EQ(to_strip(l, 9, 0, 0, 1, 1).w, 0);

  bool ok;
  CHECK_EQ(page_ranges("", 10, &ok), std::string(""));
  CHECK(ok);
  CHECK_EQ(page_ranges("All", 10, &ok), std::string(""));
  CHECK_EQ(page_ranges("1-3, 5, 8-", 10, &ok), std::string("1-3,5,8-10"));
  CHECK(ok);
  CHECK_EQ(page_ranges("-2,4", 10, &ok), std::string("1-2,4"));
  CHECK_EQ(page_ranges("7-20", 10, &ok), std::string("7-10"));
  page_ranges("11", 10, &ok);
  CHECK(!ok);
  page_ranges("3-1", 10, &ok);
  CHECK(!ok);
  page_ranges("a-b", 10, &ok);
  CHECK(!ok);
  page_ranges(",", 10, &ok);
  CHECK(!ok);
  return test_result("docs");
}
