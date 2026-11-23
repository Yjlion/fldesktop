// fl-docs: page layout, zoom, print ranges and file formats (no poppler, no
// FLTK; unit tested)
#pragma once
#include <string>
#include <vector>

namespace docs {

// Page sizes in points (1/72 inch).
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

// What a file is, from its first bytes (and its name when they do not tell).
enum class Format {
  Unknown,
  Pdf,
  Epub,
  Odt,      // OpenDocument text (and its template)
  OdfOther, // spreadsheets, presentations, ...: not supported
  Zip,      // another zip: EPUB or ODF without a leading "mimetype", or a comic book
  Archive,  // rar, 7z, tar: a comic book (.cbr, .cb7, .cbt)
  Markdown, // .md, .markdown, ... (text has no magic: by the name)
};
Format sniff(const std::string &head, const std::string &path);

// "page2.png" before "page10.png"; case-insensitive.
bool natural_less(const std::string &a, const std::string &b);

// Pixel size of a PNG, JPEG, GIF or BMP from its bytes; false when unknown.
bool image_size(const std::string &data, int &w, int &h);

// An image file name (comic book pages): png, jpg, jpeg, gif, bmp, svg.
bool is_image_name(const std::string &name);

// A CSS / ODF length ("2.5cm", "12pt", "0.5in", "10mm", "16px", "1pc") in
// points; `fallback` for anything else.
double parse_length(const std::string &text, double fallback);

// Markdown (CommonMark and GitHub's tables, strike-through and task lists)
// to HTML for the e-book layout (markdown.cpp). `title`: the front matter's
// title, else the first level 1 heading, else "".
std::string markdown_to_html(const std::string &md, std::string *title = nullptr);
// .md, .markdown, .mdown, .mkd, ...
bool is_markdown_name(const std::string &path);

} // namespace docs
