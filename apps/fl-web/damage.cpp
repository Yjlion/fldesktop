#include "damage.h"

#include <algorithm>

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

} // namespace web
