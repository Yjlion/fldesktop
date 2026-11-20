// fl-docs: page layout, zoom and print ranges (no poppler, no FLTK; unit tested)
#pragma once
#include <string>
#include <vector>

namespace docs {

// Page sizes in points (1/72 inch), as poppler reports them.
struct PageSize {
  double w = 612, h = 792;
};

// Zoom 1.0 shows a page at its real size on a 96 dpi screen.
constexpr double kPixelsPerPoint = 96.0 / 72.0;
constexpr int kGap = 12; // pixels between pages and around them

// The pages laid out top to bottom, centered, at a zoom.
struct Layout {
  double zoom = 1;
  std::vector<int> x, y, w, h; // pixels
  int width = 0, height = 0;   // the whole strip
  double scale() const { return zoom * kPixelsPerPoint; }
};
Layout layout(const std::vector<PageSize> &pages, double zoom, int min_width = 0);

// The page under y in the strip (the nearest one in a gap); -1 without pages.
int page_at(const Layout &l, int y);
// The page most of the view [top, top + height) shows.
int current_page(const Layout &l, int top, int height);

enum class Fit { None, Width, Page };
// The zoom that fits into a view of view_w x view_h pixels: Width fits the
// widest page (so no page sticks out), Page fits page `index` whole.
double fit_zoom(const std::vector<PageSize> &pages, int index, Fit fit, int view_w, int view_h);
// The next zoom step up (dir > 0) or down from zoom, within 10% .. 800%.
double zoom_step(double zoom, int dir);

// A rectangle on a page in points (top-left origin) to strip pixels.
struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};
Rect to_strip(const Layout &l, int page, double x, double y, double w, double h);

// "1-3, 5, 8-" for a document of n pages: normalized for IPP page-ranges
// ("1-3,5,8-12"); "" when the text is empty or "all"; ok=false when invalid.
std::string page_ranges(const std::string &text, int n, bool *ok);

} // namespace docs
