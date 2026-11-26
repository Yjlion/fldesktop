#include "sixel.h"

#include <algorithm>
#include <cmath>

namespace sixel {

namespace {

struct Rgb {
  uint8_t r, g, b;
};

// the VT340's 16 colours (percent values, as xterm has them)
const Rgb kDefault[16] = {{0, 0, 0},    {20, 20, 80}, {80, 13, 13}, {20, 80, 20}, {80, 20, 80}, {20, 80, 80},
                          {80, 80, 20}, {53, 53, 53}, {26, 26, 26}, {33, 33, 60}, {60, 26, 26}, {33, 60, 33},
                          {60, 33, 60}, {33, 60, 60}, {60, 60, 33}, {80, 80, 80}};

uint8_t pct(int v) { return (uint8_t)(std::max(0, std::min(100, v)) * 255 / 100); }

Rgb hls(int h, int l, int s) {
  // sixel HLS: hue 0 is blue, 120 red, 240 green
  double H = std::fmod((h + 240) % 360, 360.0) / 360.0, L = std::max(0, std::min(100, l)) / 100.0,
         S = std::max(0, std::min(100, s)) / 100.0;
  auto f = [](double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
  };
  double q = L < 0.5 ? L * (1 + S) : L + S - L * S, p = 2 * L - q;
  return {(uint8_t)std::lround(f(p, q, H + 1.0 / 3) * 255), (uint8_t)std::lround(f(p, q, H) * 255),
          (uint8_t)std::lround(f(p, q, H - 1.0 / 3) * 255)};
}

// reads a decimal number at s[i] (at most 6 digits kept)
int number(const std::string &s, size_t &i) {
  long v = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    if (v < 1000000) v = v * 10 + (s[i] - '0');
    i++;
  }
  return (int)v;
}

}  // namespace

bool decode(const std::string &params, const std::string &data, Image &out, int max_w, int max_h) {
  out = Image();
  if (max_w < 1 || max_h < 1) return false;
  (void)params;  // P2 (background) is not needed: unset pixels stay transparent
  Rgb pal[256];
  for (int i = 0; i < 256; i++) {
    const Rgb &d = kDefault[i % 16];
    pal[i] = {pct(d.r), pct(d.g), pct(d.b)};
  }
  int color = 0, x = 0, y = 0;  // y: the top of the current band of six rows
  int w = 0, h = 0;             // the size reached (or set by raster attributes)
  int cap_w = 0, cap_h = 0;     // the buffer's size
  std::vector<uint8_t> &px = out.rgba;
  auto grow = [&](int need_w, int need_h) {
    need_w = std::min(need_w, max_w);
    need_h = std::min(need_h, max_h);
    if (need_w <= cap_w && need_h <= cap_h) return;
    int nw = std::max(cap_w, std::min(max_w, std::max(need_w, cap_w * 2))),
        nh = std::max(cap_h, std::min(max_h, std::max(need_h, cap_h * 2)));
    std::vector<uint8_t> n((size_t)nw * nh * 4, 0);
    for (int r = 0; r < cap_h; r++)
      std::copy(px.begin() + (size_t)r * cap_w * 4, px.begin() + (size_t)(r + 1) * cap_w * 4,
                n.begin() + (size_t)r * nw * 4);
    px.swap(n);
    cap_w = nw;
    cap_h = nh;
  };
  auto put = [&](int bits, int count) {
    if (count < 1) count = 1;
    if (x >= max_w || y >= max_h) {
      x += count;
      return;
    }
    count = std::min(count, max_w - x);
    if (bits) {
      int top = 0;
      for (int b = 5; b >= 0; b--)
        if (bits & (1 << b)) {
          top = b;
          break;
        }
      grow(x + count, y + top + 1);
      const Rgb &c = pal[color];
      for (int b = 0; b < 6; b++) {
        if (!(bits & (1 << b)) || y + b >= cap_h) continue;
        uint8_t *p = &px[((size_t)(y + b) * cap_w + x) * 4];
        for (int k = 0; k < count && x + k < cap_w; k++, p += 4) {
          p[0] = c.r;
          p[1] = c.g;
          p[2] = c.b;
          p[3] = 255;
        }
      }
      h = std::max(h, std::min(y + top + 1, max_h));
    }
    x += count;
    w = std::max(w, std::min(x, max_w));
  };
  size_t i = 0;
  while (i < data.size()) {
    char c = data[i];
    if (c >= '?' && c <= '~') {
      put(c - '?', 1);
      i++;
    } else if (c == '!') {  // repeat: !Pn<sixel>
      i++;
      int n = number(data, i);
      if (i < data.size() && data[i] >= '?' && data[i] <= '~') put(data[i++] - '?', std::min(n, max_w));
    } else if (c == '#') {  // colour: #Pc, or #Pc;Pu;Px;Py;Pz
      i++;
      int reg = number(data, i) & 255;
      if (i < data.size() && data[i] == ';') {
        int v[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4 && i < data.size() && data[i] == ';'; k++) {
          i++;
          v[k] = number(data, i);
        }
        if (v[0] == 2) pal[reg] = {pct(v[1]), pct(v[2]), pct(v[3])};
        else if (v[0] == 1) pal[reg] = hls(v[1], v[2], v[3]);
      }
      color = reg;
    } else if (c == '"') {  // raster attributes: "Pan;Pad;Ph;Pv
      i++;
      int v[4] = {0, 0, 0, 0};
      for (int k = 0; k < 4 && i < data.size(); k++) {
        v[k] = number(data, i);
        if (i < data.size() && data[i] == ';') i++;
        else break;
      }
      if (v[2] > 0 && v[3] > 0) {
        w = std::max(w, std::min(v[2], max_w));
        h = std::max(h, std::min(v[3], max_h));
      }
    } else if (c == '$') {
      x = 0;
      i++;
    } else if (c == '-') {
      x = 0;
      y += 6;
      i++;
    } else {
      i++;  // line breaks and anything else
    }
  }
  if (w <= 0 || h <= 0) {
    out = Image();
    return false;
  }
  // the final size: what was drawn, or the raster size if larger
  std::vector<uint8_t> fin((size_t)w * h * 4, 0);
  for (int r = 0; r < std::min(h, cap_h); r++)
    std::copy(px.begin() + (size_t)r * cap_w * 4, px.begin() + (size_t)r * cap_w * 4 + (size_t)std::min(w, cap_w) * 4,
              fin.begin() + (size_t)r * w * 4);
  out.w = w;
  out.h = h;
  out.rgba.swap(fin);
  return true;
}

}  // namespace sixel
