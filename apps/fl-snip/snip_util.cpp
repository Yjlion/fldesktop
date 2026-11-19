#include "snip_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace snip {

Rect span(int x1, int y1, int x2, int y2) {
  Rect r;
  r.x = std::min(x1, x2);
  r.y = std::min(y1, y2);
  r.w = std::abs(x2 - x1) + 1;
  r.h = std::abs(y2 - y1) + 1;
  return r;
}

Rect clip(Rect r, int W, int H) {
  int x2 = std::min(r.x + r.w, W), y2 = std::min(r.y + r.h, H);
  r.x = std::max(0, r.x);
  r.y = std::max(0, r.y);
  r.w = std::max(0, x2 - r.x);
  r.h = std::max(0, y2 - r.y);
  return r;
}

Rect scale(const Rect &r, double sx, double sy) {
  Rect o;
  o.x = (int)std::floor(r.x * sx);
  o.y = (int)std::floor(r.y * sy);
  o.w = (int)std::ceil((r.x + r.w) * sx) - o.x;
  o.h = (int)std::ceil((r.y + r.h) * sy) - o.y;
  return o;
}

std::vector<Window> parse_windows(const std::string &text) {
  std::vector<Window> out;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    Window w;
    int n = 0;
    if (sscanf(line.c_str(), "%d %d %d %d %n", &w.r.x, &w.r.y, &w.r.w, &w.r.h, &n) < 4) continue;
    if (w.r.empty()) continue;
    w.title = n > 0 && n <= (int)line.size() ? line.substr((size_t)n) : "";
    out.push_back(w);
  }
  return out;
}

int window_at(const std::vector<Window> &wins, int x, int y) {
  for (size_t i = 0; i < wins.size(); i++)
    if (wins[i].r.contains(x, y)) return (int)i;
  return -1;
}

bool inside_polygon(const std::vector<Point> &poly, double x, double y) {
  bool in = false;
  size_t n = poly.size();
  for (size_t i = 0, j = n - 1; i < n; j = i++) {
    double xi = poly[i].first, yi = poly[i].second, xj = poly[j].first, yj = poly[j].second;
    if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
  }
  return in;
}

Rect bounds(const std::vector<Point> &poly) {
  if (poly.empty()) return {};
  int x1 = poly[0].first, y1 = poly[0].second, x2 = x1, y2 = y1;
  for (auto &p : poly) {
    x1 = std::min(x1, p.first);
    y1 = std::min(y1, p.second);
    x2 = std::max(x2, p.first);
    y2 = std::max(y2, p.second);
  }
  return span(x1, y1, x2, y2);
}

// reads a whitespace separated number, skipping # comments
static bool ppm_number(const std::string &d, size_t &i, int &v) {
  for (;;) {
    while (i < d.size() && isspace((unsigned char)d[i])) i++;
    if (i < d.size() && d[i] == '#') {
      while (i < d.size() && d[i] != '\n') i++;
      continue;
    }
    break;
  }
  if (i >= d.size() || !isdigit((unsigned char)d[i])) return false;
  long n = 0;
  while (i < d.size() && isdigit((unsigned char)d[i])) {
    n = n * 10 + (d[i++] - '0');
    if (n > 1 << 20) return false;
  }
  v = (int)n;
  return true;
}

bool parse_ppm(const std::string &d, Image &out) {
  if (d.size() < 2 || d[0] != 'P' || d[1] != '6') return false;
  size_t i = 2;
  int w, h, maxval;
  if (!ppm_number(d, i, w) || !ppm_number(d, i, h) || !ppm_number(d, i, maxval)) return false;
  if (w <= 0 || h <= 0 || maxval != 255) return false;
  i++; // the single whitespace after maxval
  size_t need = (size_t)w * h * 3;
  if (d.size() < i + need) return false;
  out.w = w;
  out.h = h;
  out.px.resize((size_t)w * h * 4);
  const uint8_t *s = (const uint8_t *)d.data() + i;
  for (size_t k = 0; k < (size_t)w * h; k++) {
    out.px[k * 4] = s[k * 3];
    out.px[k * 4 + 1] = s[k * 3 + 1];
    out.px[k * 4 + 2] = s[k * 3 + 2];
    out.px[k * 4 + 3] = 255;
  }
  return true;
}

Image crop(const Image &img, Rect r) {
  r = clip(r, img.w, img.h);
  Image o;
  if (r.empty()) return o;
  o.w = r.w;
  o.h = r.h;
  o.px.resize((size_t)r.w * r.h * 4);
  for (int y = 0; y < r.h; y++)
    std::copy_n(img.at(r.x, r.y + y), (size_t)r.w * 4, o.px.data() + (size_t)y * r.w * 4);
  return o;
}

void mask_outside(Image &img, const std::vector<Point> &poly) {
  if (poly.size() < 3) return;
  for (int y = 0; y < img.h; y++)
    for (int x = 0; x < img.w; x++)
      if (!inside_polygon(poly, x + 0.5, y + 0.5)) img.at(x, y)[3] = 0;
}

void pixelate(Image &img, Rect r, int block) {
  r = clip(r, img.w, img.h);
  if (r.empty() || block < 2) return;
  for (int by = r.y; by < r.y + r.h; by += block)
    for (int bx = r.x; bx < r.x + r.w; bx += block) {
      int x2 = std::min(bx + block, r.x + r.w), y2 = std::min(by + block, r.y + r.h);
      unsigned long sum[4] = {0, 0, 0, 0}, n = 0;
      for (int y = by; y < y2; y++)
        for (int x = bx; x < x2; x++, n++)
          for (int c = 0; c < 4; c++) sum[c] += img.at(x, y)[c];
      for (int y = by; y < y2; y++)
        for (int x = bx; x < x2; x++)
          for (int c = 0; c < 4; c++) img.at(x, y)[c] = (uint8_t)(sum[c] / n);
    }
}

std::string file_name(time_t t) {
  char buf[64];
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, sizeof buf, "Screenshot-%Y-%m-%d-%H%M%S.png", &tm);
  return buf;
}

} // namespace snip
