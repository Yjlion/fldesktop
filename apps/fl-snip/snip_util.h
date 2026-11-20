// fl-snip helpers without a display: rectangles, the window list from fl-wm,
// freeform masks, PPM decoding and file names (unit tested).
#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

namespace snip {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
  bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
  bool operator==(const Rect &o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// the rectangle between two corners (in any order), corners included
Rect span(int x1, int y1, int x2, int y2);
// r cut to 0,0..W,H
Rect clip(Rect r, int W, int H);
// r scaled by sx, sy (layout to image pixels), outward to whole pixels
Rect scale(const Rect &r, double sx, double sy);

// fl-wm's window list ($XDG_RUNTIME_DIR/fl-wm-windows): "x y w h title" lines,
// topmost first, in layout coordinates
struct Window {
  Rect r;
  std::string title;
};
std::vector<Window> parse_windows(const std::string &text);
// the topmost window at x, y (layout coordinates), -1 for none
int window_at(const std::vector<Window> &wins, int x, int y);

using Point = std::pair<int, int>;
bool inside_polygon(const std::vector<Point> &poly, double x, double y);
Rect bounds(const std::vector<Point> &poly);

// An RGBA picture (row-major, 4 bytes a pixel)
struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
  bool empty() const { return w <= 0 || h <= 0; }
  uint8_t *at(int x, int y) { return px.data() + ((size_t)y * w + x) * 4; }
  const uint8_t *at(int x, int y) const { return px.data() + ((size_t)y * w + x) * 4; }
};
// binary PPM (P6, maxval 255), as grim writes it
bool parse_ppm(const std::string &data, Image &out);
Image crop(const Image &img, Rect r);
// pixels outside the polygon (image coordinates, relative to the crop) get
// alpha 0
void mask_outside(Image &img, const std::vector<Point> &poly);
// blocks of `block` pixels averaged (for hiding text)
void pixelate(Image &img, Rect r, int block);

// "Screenshot-2026-11-19-143005.png"
std::string file_name(time_t t);
// "Screencast-2026-11-19-221005.webm"
std::string recording_name(time_t t);

// The GStreamer pipeline that records a PipeWire video node (from the
// ScreenCast portal; fd < 0: the default PipeWire) as VP8 in WebM, at a
// steady 30 frames a second (the screen sends frames only when it changes),
// with the desktop's sound as Opus when audio is set. The file goes to the
// element named "sink" (its location is set separately).
std::string record_pipeline(int fd, uint32_t node, bool audio);

} // namespace snip
