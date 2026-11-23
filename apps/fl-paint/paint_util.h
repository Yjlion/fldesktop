// fl-paint: the raster work (no FLTK; unit tested). Pixels are straight
// RGBA, one uint32_t each as 0xAARRGGBB.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace paint {

using Color = uint32_t; // 0xAARRGGBB
inline Color rgba(int r, int g, int b, int a = 255) {
  return ((Color)a << 24) | ((Color)r << 16) | ((Color)g << 8) | (Color)b;
}
inline int ch_a(Color c) { return (int)(c >> 24); }
inline int ch_r(Color c) { return (int)((c >> 16) & 255); }
inline int ch_g(Color c) { return (int)((c >> 8) & 255); }
inline int ch_b(Color c) { return (int)(c & 255); }

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};
// the rectangle between two corners (either order), clipped to W x H
Rect span(int x0, int y0, int x1, int y1, int W, int H);

class Canvas {
public:
  int w = 0, h = 0;
  std::vector<Color> px;
  Canvas() = default;
  Canvas(int W, int H, Color fill = 0);
  Color get(int x, int y) const { return x >= 0 && y >= 0 && x < w && y < h ? px[(size_t)y * w + x] : 0; }
  void set(int x, int y, Color c) {
    if (x >= 0 && y >= 0 && x < w && y < h) px[(size_t)y * w + x] = c;
  }
  // paints c over the pixel with `cover` (0..1) of its alpha ("source over")
  void blend(int x, int y, Color c, double cover = 1.0);
  // erases: lowers the pixel's alpha by `cover`
  void erase(int x, int y, double cover = 1.0);
};

// "source over" of one colour on another (straight alpha)
Color over(Color top, Color bottom);

// A round brush of `size` pixels from (x0,y0) to (x1,y1), soft edged.
// erase: rubs out instead of painting.
void stroke(Canvas &c, double x0, double y0, double x1, double y1, Color col, double size, bool erase = false);
void line(Canvas &c, double x0, double y0, double x1, double y1, Color col, double width);
// outlines with `width`, or filled (fill = true)
void rectangle(Canvas &c, Rect r, Color col, double width, bool fill);
void ellipse(Canvas &c, Rect r, Color col, double width, bool fill);
// fills the area around (x,y) of colours within `tolerance` (0..255) of it
void flood_fill(Canvas &c, int x, int y, Color col, int tolerance = 0);

Canvas crop(const Canvas &c, Rect r);
// a bigger or smaller canvas, the picture at (dx,dy), the rest `fill`
Canvas resize_canvas(const Canvas &c, int W, int H, int dx, int dy, Color fill);
// the picture scaled to W x H (bilinear)
Canvas scale(const Canvas &c, int W, int H);
Canvas flip(const Canvas &c, bool horizontal);
Canvas rotate90(const Canvas &c, bool clockwise);
// pastes `src` at (x,y) over `dst`
void draw_over(Canvas &dst, const Canvas &src, int x, int y, double opacity = 1.0);

struct Layer {
  std::string name;
  Canvas c;
  bool visible = true;
  double opacity = 1.0;
};
// the visible layers, bottom first, over `background`
Canvas flatten(const std::vector<Layer> &layers, Color background);

// the colour as "#rrggbb" (or "#rrggbbaa" when not opaque) and back
std::string to_hex(Color c);
bool from_hex(const std::string &s, Color &c);

} // namespace paint
