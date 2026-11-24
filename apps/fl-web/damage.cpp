#include "damage.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace web {

std::vector<Rect> clip_damage(const Rect *rects, unsigned n, int width, int height, bool &full, unsigned max) {
  std::vector<Rect> out;
  full = n == 0;
  if (full) return out;
  for (unsigned i = 0; i < n; i++) {
    int x0 = std::max(0, rects[i].x), y0 = std::max(0, rects[i].y);
    int x1 = std::min(width, rects[i].x + rects[i].w), y1 = std::min(height, rects[i].y + rects[i].h);
    if (x1 > x0 && y1 > y0) out.push_back({x0, y0, x1 - x0, y1 - y0});
  }
  if (out.size() > max) {
    Rect b = out[0];
    for (const Rect &r : out) {
      int x1 = std::max(b.x + b.w, r.x + r.w), y1 = std::max(b.y + b.h, r.y + r.h);
      b.x = std::min(b.x, r.x);
      b.y = std::min(b.y, r.y);
      b.w = x1 - b.x;
      b.h = y1 - b.y;
    }
    out = {b};
  }
  for (const Rect &r : out)
    if (r.x == 0 && r.y == 0 && r.w == width && r.h == height) {
      full = true;
      out.clear();
      break;
    }
  return out;
}

std::vector<Rect> diff_damage(const unsigned char *a, const unsigned char *b, int width, int height, int stride,
                              bool &full, unsigned max) {
  std::vector<Rect> bands;
  full = false;
  const size_t row_bytes = (size_t)width * 4;
  Rect cur;
  bool open = false;
  for (int y = 0; y < height; y++) {
    const unsigned char *ra = a + (size_t)y * stride, *rb = b + (size_t)y * stride;
    if (memcmp(ra, rb, row_bytes) == 0) {
      if (open) bands.push_back(cur);
      open = false;
      continue;
    }
    // the first and last changed pixel of the row
    int x0 = 0, x1 = width;
    while (x0 < width && memcmp(ra + (size_t)x0 * 4, rb + (size_t)x0 * 4, 4) == 0) x0++;
    while (x1 > x0 && memcmp(ra + (size_t)(x1 - 1) * 4, rb + (size_t)(x1 - 1) * 4, 4) == 0) x1--;
    if (!open) {
      cur = {x0, y, x1 - x0, 1};
      open = true;
    } else {
      int r = std::max(cur.x + cur.w, x1);
      cur.x = std::min(cur.x, x0);
      cur.w = r - cur.x;
      cur.h = y + 1 - cur.y;
    }
  }
  if (open) bands.push_back(cur);
  // too many bands: merge the two closest neighbours until they fit
  while (bands.size() > std::max(1u, max)) {
    size_t best = 0;
    int gap = INT32_MAX;
    for (size_t i = 0; i + 1 < bands.size(); i++) {
      int g = bands[i + 1].y - (bands[i].y + bands[i].h);
      if (g < gap) { gap = g; best = i; }
    }
    Rect &m = bands[best], &n = bands[best + 1];
    int r = std::max(m.x + m.w, n.x + n.w);
    m.x = std::min(m.x, n.x);
    m.w = r - m.x;
    m.h = n.y + n.h - m.y;
    bands.erase(bands.begin() + (long)best + 1);
  }
  if (bands.size() == 1 && bands[0].x == 0 && bands[0].y == 0 && bands[0].w == width && bands[0].h == height) {
    full = true;
    bands.clear();
  }
  return bands;
}

} // namespace web
