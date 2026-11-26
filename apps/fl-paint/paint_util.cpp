#include "paint_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace paint {

Rect span(int x0, int y0, int x1, int y1, int W, int H) {
  int a = std::max(0, std::min(x0, x1)), b = std::max(0, std::min(y0, y1));
  int c = std::min(W, std::max(x0, x1) + 1), d = std::min(H, std::max(y0, y1) + 1);
  return {a, b, std::max(0, c - a), std::max(0, d - b)};
}

Canvas::Canvas(int W, int H, Color fill) : w(std::max(0, W)), h(std::max(0, H)), px((size_t)w * h, fill) {}

Color over(Color top, Color bottom) {
  double ta = ch_a(top) / 255.0, ba = ch_a(bottom) / 255.0;
  double oa = ta + ba * (1 - ta);
  if (oa <= 0) return 0;
  auto mix = [&](int t, int b) { return (int)std::lround((t * ta + b * ba * (1 - ta)) / oa); };
  return rgba(mix(ch_r(top), ch_r(bottom)), mix(ch_g(top), ch_g(bottom)), mix(ch_b(top), ch_b(bottom)),
              (int)std::lround(oa * 255));
}

void Canvas::blend(int x, int y, Color c, double cover) {
  if (x < 0 || y < 0 || x >= w || y >= h || cover <= 0) return;
  int a = (int)std::lround(ch_a(c) * std::min(1.0, cover));
  Color &p = px[(size_t)y * w + x];
  p = over((c & 0x00ffffff) | ((Color)a << 24), p);
}

void Canvas::erase(int x, int y, double cover) {
  if (x < 0 || y < 0 || x >= w || y >= h || cover <= 0) return;
  Color &p = px[(size_t)y * w + x];
  int a = (int)std::lround(ch_a(p) * (1 - std::min(1.0, cover)));
  p = (p & 0x00ffffff) | ((Color)a << 24);
}

// the coverage of a pixel by a disc: soft over the last pixel of the radius
static double disc_cover(double d, double r) {
  if (d <= r - 0.5) return 1.0;
  if (d >= r + 0.5) return 0.0;
  return r + 0.5 - d;
}

double pressure_size(double size, double pressure) {
  if (!(pressure >= 0)) pressure = 0;  // NaN too
  if (pressure > 1) pressure = 1;
  return std::max(1.0, size * (0.2 + 0.8 * pressure));
}

void stroke(Canvas &c, double x0, double y0, double x1, double y1, Color col, double size, bool erase) {
  double r = std::max(0.5, size / 2);
  // the distance of each pixel to the segment: one pass over its bounding box
  int minx = (int)std::floor(std::min(x0, x1) - r - 1), maxx = (int)std::ceil(std::max(x0, x1) + r + 1);
  int miny = (int)std::floor(std::min(y0, y1) - r - 1), maxy = (int)std::ceil(std::max(y0, y1) + r + 1);
  minx = std::max(0, minx);
  miny = std::max(0, miny);
  maxx = std::min(c.w - 1, maxx);
  maxy = std::min(c.h - 1, maxy);
  double dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
  for (int y = miny; y <= maxy; y++)
    for (int x = minx; x <= maxx; x++) {
      double px = x + 0.5, py = y + 0.5;
      double t = len2 > 0 ? std::max(0.0, std::min(1.0, ((px - x0) * dx + (py - y0) * dy) / len2)) : 0;
      double ex = x0 + t * dx - px, ey = y0 + t * dy - py;
      double cov = disc_cover(std::sqrt(ex * ex + ey * ey), r);
      if (cov <= 0) continue;
      if (erase) c.erase(x, y, cov);
      else c.blend(x, y, col, cov);
    }
}

void line(Canvas &c, double x0, double y0, double x1, double y1, Color col, double width) {
  stroke(c, x0, y0, x1, y1, col, width, false);
}

void rectangle(Canvas &c, Rect r, Color col, double width, bool fill) {
  if (r.empty()) return;
  if (fill) {
    for (int y = r.y; y < r.y + r.h; y++)
      for (int x = r.x; x < r.x + r.w; x++) c.blend(x, y, col);
    return;
  }
  // a frame `width` thick inside the rectangle
  int t = std::max(1, (int)std::lround(width));
  for (int y = r.y; y < r.y + r.h; y++)
    for (int x = r.x; x < r.x + r.w; x++)
      if (x < r.x + t || x >= r.x + r.w - t || y < r.y + t || y >= r.y + r.h - t) c.blend(x, y, col);
}

void ellipse(Canvas &c, Rect r, Color col, double width, bool fill) {
  if (r.empty()) return;
  double cx = r.x + r.w / 2.0, cy = r.y + r.h / 2.0, rx = r.w / 2.0, ry = r.h / 2.0;
  double t = std::max(1.0, width);
  for (int y = r.y; y < r.y + r.h; y++)
    for (int x = r.x; x < r.x + r.w; x++) {
      // 4 samples per pixel for soft edges
      int in = 0, ring = 0;
      for (int s = 0; s < 4; s++) {
        double px = x + 0.25 + 0.5 * (s & 1), py = y + 0.25 + 0.5 * (s >> 1);
        double nx = (px - cx) / rx, ny = (py - cy) / ry;
        double d = nx * nx + ny * ny;
        if (d <= 1) {
          in++;
          double ix = (px - cx) / std::max(0.5, rx - t), iy = (py - cy) / std::max(0.5, ry - t);
          if (rx <= t || ry <= t || ix * ix + iy * iy > 1) ring++;
        }
      }
      int n = fill ? in : ring;
      if (n) c.blend(x, y, col, n / 4.0);
    }
}

static int color_dist(Color a, Color b) {
  return std::max({std::abs(ch_r(a) - ch_r(b)), std::abs(ch_g(a) - ch_g(b)), std::abs(ch_b(a) - ch_b(b)),
                   std::abs(ch_a(a) - ch_a(b))});
}

void flood_fill(Canvas &c, int x, int y, Color col, int tolerance) {
  if (x < 0 || y < 0 || x >= c.w || y >= c.h) return;
  Color target = c.get(x, y);
  if (target == col && tolerance == 0) return;
  std::vector<uint8_t> seen((size_t)c.w * c.h, 0);
  std::vector<std::pair<int, int>> stack = {{x, y}};
  while (!stack.empty()) {
    auto [sx, sy] = stack.back();
    stack.pop_back();
    // a horizontal run, then the rows above and below
    int lx = sx, rx = sx;
    auto fits = [&](int px, int py) {
      return !seen[(size_t)py * c.w + px] && color_dist(c.get(px, py), target) <= tolerance;
    };
    if (!fits(sx, sy)) continue;
    while (lx > 0 && fits(lx - 1, sy)) lx--;
    while (rx < c.w - 1 && fits(rx + 1, sy)) rx++;
    for (int px = lx; px <= rx; px++) {
      seen[(size_t)sy * c.w + px] = 1;
      c.set(px, sy, col);
      if (sy > 0 && fits(px, sy - 1)) stack.push_back({px, sy - 1});
      if (sy < c.h - 1 && fits(px, sy + 1)) stack.push_back({px, sy + 1});
    }
  }
}

Canvas crop(const Canvas &c, Rect r) {
  r = span(r.x, r.y, r.x + r.w - 1, r.y + r.h - 1, c.w, c.h);
  Canvas out(r.w, r.h);
  for (int y = 0; y < r.h; y++)
    for (int x = 0; x < r.w; x++) out.px[(size_t)y * r.w + x] = c.get(r.x + x, r.y + y);
  return out;
}

Canvas resize_canvas(const Canvas &c, int W, int H, int dx, int dy, Color fill) {
  Canvas out(W, H, fill);
  for (int y = 0; y < c.h; y++)
    for (int x = 0; x < c.w; x++) out.set(x + dx, y + dy, c.get(x, y));
  return out;
}

Canvas scale(const Canvas &c, int W, int H) {
  Canvas out(W, H);
  if (c.w == 0 || c.h == 0) return out;
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      double sx = (x + 0.5) * c.w / W - 0.5, sy = (y + 0.5) * c.h / H - 0.5;
      int x0 = (int)std::floor(sx), y0 = (int)std::floor(sy);
      double fx = sx - x0, fy = sy - y0;
      auto at = [&](int px, int py) {
        return c.get(std::max(0, std::min(c.w - 1, px)), std::max(0, std::min(c.h - 1, py)));
      };
      Color p[4] = {at(x0, y0), at(x0 + 1, y0), at(x0, y0 + 1), at(x0 + 1, y0 + 1)};
      double wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
      double a = 0, r = 0, g = 0, b = 0;
      for (int i = 0; i < 4; i++) {
        double pa = ch_a(p[i]) * wts[i];
        a += pa;
        r += ch_r(p[i]) * pa;
        g += ch_g(p[i]) * pa;
        b += ch_b(p[i]) * pa;
      }
      if (a <= 0) continue;
      out.px[(size_t)y * W + x] = rgba((int)std::lround(r / a), (int)std::lround(g / a), (int)std::lround(b / a),
                                       (int)std::lround(a));
    }
  return out;
}

Canvas flip(const Canvas &c, bool horizontal) {
  Canvas out(c.w, c.h);
  for (int y = 0; y < c.h; y++)
    for (int x = 0; x < c.w; x++)
      out.px[(size_t)y * c.w + x] = horizontal ? c.get(c.w - 1 - x, y) : c.get(x, c.h - 1 - y);
  return out;
}

Canvas rotate90(const Canvas &c, bool cw) {
  Canvas out(c.h, c.w);
  for (int y = 0; y < c.h; y++)
    for (int x = 0; x < c.w; x++) {
      if (cw) out.set(c.h - 1 - y, x, c.get(x, y));
      else out.set(y, c.w - 1 - x, c.get(x, y));
    }
  return out;
}

void draw_over(Canvas &dst, const Canvas &src, int ox, int oy, double opacity) {
  for (int y = 0; y < src.h; y++)
    for (int x = 0; x < src.w; x++) {
      Color s = src.px[(size_t)y * src.w + x];
      if (ch_a(s)) dst.blend(x + ox, y + oy, s, opacity);
    }
}

Canvas flatten(const std::vector<Layer> &layers, Color background) {
  int W = layers.empty() ? 0 : layers[0].c.w, H = layers.empty() ? 0 : layers[0].c.h;
  Canvas out(W, H, background);
  for (const Layer &l : layers)
    if (l.visible) draw_over(out, l.c, 0, 0, l.opacity);
  return out;
}

std::string to_hex(Color c) {
  char b[16];
  if (ch_a(c) == 255) snprintf(b, sizeof b, "#%02x%02x%02x", ch_r(c), ch_g(c), ch_b(c));
  else snprintf(b, sizeof b, "#%02x%02x%02x%02x", ch_r(c), ch_g(c), ch_b(c), ch_a(c));
  return b;
}

bool from_hex(const std::string &s, Color &c) {
  if (s.empty() || s[0] != '#') return false;
  std::string h = s.substr(1);
  if (h.size() != 6 && h.size() != 8) return false;
  for (char ch : h)
    if (!isxdigit((unsigned char)ch)) return false;
  unsigned long v = strtoul(h.c_str(), nullptr, 16);
  c = h.size() == 6 ? (Color)(0xff000000u | v) : (Color)(((v & 0xff) << 24) | (v >> 8));
  return true;
}

} // namespace paint
