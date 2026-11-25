#include "view_util.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace view {

// ---- EXIF ----------------------------------------------------------------------------

namespace {

struct Tiff {
  const unsigned char *p;
  size_t n;
  bool le;
  unsigned u16(size_t o) const {
    if (o + 2 > n) return 0;
    return le ? p[o] | p[o + 1] << 8 : p[o] << 8 | p[o + 1];
  }
  uint32_t u32(size_t o) const {
    if (o + 4 > n) return 0;
    return le ? (uint32_t)p[o] | (uint32_t)p[o + 1] << 8 | (uint32_t)p[o + 2] << 16 | (uint32_t)p[o + 3] << 24
              : (uint32_t)p[o] << 24 | (uint32_t)p[o + 1] << 16 | (uint32_t)p[o + 2] << 8 | (uint32_t)p[o + 3];
  }
};

struct Entry {
  unsigned tag, type;
  uint32_t count;
  size_t value_off; // where the value is (inline or pointed to)
};

size_t type_size(unsigned t) {
  switch (t) {
    case 1: case 2: case 6: case 7: return 1;
    case 3: case 8: return 2;
    case 4: case 9: case 11: return 4;
    case 5: case 10: case 12: return 8;
  }
  return 0;
}

std::vector<Entry> ifd(const Tiff &t, size_t off, size_t *next = nullptr) {
  std::vector<Entry> out;
  if (off + 2 > t.n) return out;
  unsigned cnt = t.u16(off);
  if (cnt > 1000) return out;
  for (unsigned i = 0; i < cnt; i++) {
    size_t e = off + 2 + i * 12;
    if (e + 12 > t.n) break;
    Entry en{t.u16(e), t.u16(e + 2), t.u32(e + 4), e + 8};
    size_t sz = type_size(en.type) * en.count;
    if (sz > 4) en.value_off = t.u32(e + 8);
    if (sz == 0 || en.value_off + sz > t.n) continue;
    out.push_back(en);
  }
  if (next) *next = t.u32(off + 2 + cnt * 12);
  return out;
}

std::string str(const Tiff &t, const Entry &e) {
  if (e.type != 2 && e.type != 7) return "";
  std::string s((const char *)t.p + e.value_off, e.count);
  s = s.substr(0, s.find('\0'));
  while (!s.empty() && (s.back() == ' ')) s.pop_back();
  return s;
}

double num(const Tiff &t, const Entry &e, uint32_t i = 0) {
  size_t o = e.value_off + i * type_size(e.type);
  switch (e.type) {
    case 1: case 7: return t.p[o];
    case 3: return t.u16(o);
    case 4: return t.u32(o);
    case 8: return (int16_t)t.u16(o);
    case 9: return (int32_t)t.u32(o);
    case 5: {
      uint32_t a = t.u32(o), b = t.u32(o + 4);
      return b ? (double)a / b : 0;
    }
    case 10: {
      int32_t a = (int32_t)t.u32(o), b = (int32_t)t.u32(o + 4);
      return b ? (double)a / b : 0;
    }
  }
  return 0;
}

double dms(const Tiff &t, const Entry &e) {
  if (e.count < 3) return 0;
  return num(t, e, 0) + num(t, e, 1) / 60 + num(t, e, 2) / 3600;
}

void parse_tiff(const unsigned char *p, size_t n, Exif &x) {
  if (n < 8) return;
  Tiff t{p, n, p[0] == 'I'};
  if (!((p[0] == 'I' && p[1] == 'I') || (p[0] == 'M' && p[1] == 'M')) || t.u16(2) != 42) return;
  x.found = true;
  size_t exif_off = 0, gps_off = 0;
  for (auto &e : ifd(t, t.u32(4))) {
    switch (e.tag) {
      case 0x010F: x.make = str(t, e); break;
      case 0x0110: x.model = str(t, e); break;
      case 0x0112: x.orientation = (int)num(t, e); break;
      case 0x0131: x.software = str(t, e); break;
      case 0x0132: if (x.taken.empty()) x.taken = str(t, e); break;
      case 0x8769: exif_off = (size_t)num(t, e); break;
      case 0x8825: gps_off = (size_t)num(t, e); break;
    }
  }
  if (exif_off)
    for (auto &e : ifd(t, exif_off)) {
      switch (e.tag) {
        case 0x829A: x.exposure = num(t, e); break;
        case 0x829D: x.fnumber = num(t, e); break;
        case 0x8827: x.iso = (int)num(t, e); break;
        case 0x9003: x.taken = str(t, e); break;
        case 0x9209: x.flash = (int)num(t, e); break;
        case 0x920A: x.focal = num(t, e); break;
        case 0xA405: x.focal35 = (int)num(t, e); break;
        case 0xA002: x.width = (int)num(t, e); break;
        case 0xA003: x.height = (int)num(t, e); break;
        case 0xA434: x.lens = str(t, e); break;
      }
    }
  if (gps_off) {
    std::string lat_ref, lon_ref;
    bool have_lat = false, have_lon = false;
    int alt_ref = 0;
    for (auto &e : ifd(t, gps_off)) {
      switch (e.tag) {
        case 1: lat_ref = str(t, e); break;
        case 2: x.lat = dms(t, e); have_lat = true; break;
        case 3: lon_ref = str(t, e); break;
        case 4: x.lon = dms(t, e); have_lon = true; break;
        case 5: alt_ref = (int)num(t, e); break;
        case 6: x.alt = num(t, e); break;
      }
    }
    if (lat_ref == "S") x.lat = -x.lat;
    if (lon_ref == "W") x.lon = -x.lon;
    if (alt_ref == 1) x.alt = -x.alt;
    x.has_gps = have_lat && have_lon;
  }
  if (x.orientation < 1 || x.orientation > 8) x.orientation = 1;
}

} // namespace

Exif parse_exif(const unsigned char *d, size_t n) {
  Exif x;
  if (n >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
    // JPEG: walk the markers to APP1 "Exif\0\0"
    size_t o = 2;
    while (o + 4 <= n && d[o] == 0xFF) {
      unsigned m = d[o + 1];
      if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) {
        o += 2;
        continue;
      }
      if (m == 0xDA || m == 0xD9) break; // image data: no more metadata
      size_t len = (size_t)d[o + 2] << 8 | d[o + 3];
      if (len < 2 || o + 2 + len > n) break;
      if (m == 0xE1 && len >= 8 && memcmp(d + o + 4, "Exif\0\0", 6) == 0) {
        parse_tiff(d + o + 10, len - 8, x);
        return x;
      }
      o += 2 + len;
    }
    return x;
  }
  if (n >= 6 && memcmp(d, "Exif\0\0", 6) == 0) {
    parse_tiff(d + 6, n - 6, x);
    return x;
  }
  parse_tiff(d, n, x); // a TIFF file
  return x;
}

Exif read_exif(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::vector<unsigned char> buf(256 * 1024);
  f.read((char *)buf.data(), (std::streamsize)buf.size());
  buf.resize((size_t)f.gcount());
  return parse_exif(buf.data(), buf.size());
}

std::string exposure_text(double s) {
  if (s <= 0) return "";
  char b[32];
  if (s >= 1) snprintf(b, sizeof b, "%g s", std::round(s * 10) / 10);
  else snprintf(b, sizeof b, "1/%.0f s", 1 / s);
  return b;
}

std::vector<std::pair<std::string, std::string>> exif_rows(const Exif &e) {
  std::vector<std::pair<std::string, std::string>> r;
  if (!e.found) return r;
  std::string cam = e.model;
  // "Canon" + "Canon EOS R6" is said once
  if (!e.make.empty() && cam.compare(0, e.make.size(), e.make) != 0) cam = e.make + " " + cam;
  if (!cam.empty()) r.push_back({"Camera", cam});
  if (!e.lens.empty()) r.push_back({"Lens", e.lens});
  if (!e.taken.empty()) {
    std::string t = e.taken;
    if (t.size() >= 10 && t[4] == ':' && t[7] == ':') t[4] = t[7] = '-';
    r.push_back({"Taken", t});
  }
  std::string ex;
  char b[64];
  if (e.exposure > 0) ex += exposure_text(e.exposure);
  if (e.fnumber > 0) {
    snprintf(b, sizeof b, "%sf/%.1f", ex.empty() ? "" : "  ", e.fnumber);
    ex += b;
  }
  if (e.iso > 0) ex += (ex.empty() ? "" : "  ") + std::string("ISO ") + std::to_string(e.iso);
  if (!ex.empty()) r.push_back({"Exposure", ex});
  if (e.focal > 0) {
    snprintf(b, sizeof b, "%.0f mm", e.focal);
    std::string f = b;
    if (e.focal35 > 0 && e.focal35 != (int)std::lround(e.focal)) f += " (" + std::to_string(e.focal35) + " mm in 35 mm)";
    r.push_back({"Focal length", f});
  }
  if (e.flash >= 0) r.push_back({"Flash", (e.flash & 1) ? "Fired" : "Did not fire"});
  if (e.has_gps) {
    snprintf(b, sizeof b, "%.5f°%s, %.5f°%s", std::fabs(e.lat), e.lat < 0 ? "S" : "N", std::fabs(e.lon),
             e.lon < 0 ? "W" : "E");
    std::string g = b;
    if (e.alt != 0) {
      snprintf(b, sizeof b, ", %.0f m", e.alt);
      g += b;
    }
    r.push_back({"Location", g});
  }
  if (!e.software.empty()) r.push_back({"Software", e.software});
  return r;
}

void orientation_transform(int o, int &rotate, bool &flip) {
  // 1 upright, 2 mirrored, 3 180°, 4 180° mirrored, 5 mirrored + 90° ccw ... as in the EXIF spec
  static const int rot[9] = {0, 0, 0, 180, 180, 270, 90, 90, 270};
  static const bool fl[9] = {false, false, true, false, true, true, false, true, false};
  if (o < 1 || o > 8) o = 1;
  rotate = rot[o];
  flip = fl[o];
}

// ---- pixels --------------------------------------------------------------------------

Pixels crop(const Pixels &p, int x, int y, int w, int h) {
  x = std::max(0, std::min(x, p.w));
  y = std::max(0, std::min(y, p.h));
  w = std::max(0, std::min(w, p.w - x));
  h = std::max(0, std::min(h, p.h - y));
  Pixels out;
  out.w = w;
  out.h = h;
  out.d = p.d;
  out.data.resize((size_t)w * h * p.d);
  for (int r = 0; r < h; r++)
    memcpy(out.data.data() + (size_t)r * w * p.d, p.data.data() + ((size_t)(y + r) * p.w + x) * p.d, (size_t)w * p.d);
  return out;
}

Pixels resize(const Pixels &p, int w, int h) { return resize(p.data.data(), p.w, p.h, p.d, p.w * p.d, w, h); }

Pixels resize(const unsigned char *data, int src_w, int src_h, int d, int ld, int w, int h) {
  struct Src {
    const unsigned char *data;
    int w, h, d, ld;
  } p{data, src_w, src_h, d, ld};
  Pixels out;
  out.w = std::max(1, w);
  out.h = std::max(1, h);
  out.d = p.d;
  out.data.assign((size_t)out.w * out.h * p.d, 0);
  if (p.w <= 0 || p.h <= 0 || !data) return out;
  double sx = (double)p.w / out.w, sy = (double)p.h / out.h;
  for (int y = 0; y < out.h; y++) {
    for (int x = 0; x < out.w; x++) {
      unsigned char *o = out.data.data() + ((size_t)y * out.w + x) * p.d;
      if (sx >= 1 && sy >= 1) {
        // the average of the source area
        double x0 = x * sx, x1 = (x + 1) * sx, y0 = y * sy, y1 = (y + 1) * sy;
        double acc[4] = {0, 0, 0, 0}, wsum = 0;
        for (int yy = (int)y0; yy < std::min(p.h, (int)std::ceil(y1)); yy++) {
          double wy = std::min(y1, yy + 1.0) - std::max(y0, (double)yy);
          for (int xx = (int)x0; xx < std::min(p.w, (int)std::ceil(x1)); xx++) {
            double wx = std::min(x1, xx + 1.0) - std::max(x0, (double)xx), wt = wx * wy;
            const unsigned char *s = p.data + (size_t)yy * p.ld + (size_t)xx * p.d;
            for (int c = 0; c < p.d; c++) acc[c] += s[c] * wt;
            wsum += wt;
          }
        }
        for (int c = 0; c < p.d; c++) o[c] = (unsigned char)std::lround(wsum > 0 ? acc[c] / wsum : 0);
      } else {
        // bilinear
        double fx = std::max(0.0, (x + 0.5) * sx - 0.5), fy = std::max(0.0, (y + 0.5) * sy - 0.5);
        int x0 = std::min((int)fx, p.w - 1), y0 = std::min((int)fy, p.h - 1);
        int x1 = std::min(x0 + 1, p.w - 1), y1 = std::min(y0 + 1, p.h - 1);
        double ax = fx - x0, ay = fy - y0;
        for (int c = 0; c < p.d; c++) {
          auto px = [&](int xx, int yy) { return (double)p.data[(size_t)yy * p.ld + (size_t)xx * p.d + c]; };
          double v = (px(x0, y0) * (1 - ax) + px(x1, y0) * ax) * (1 - ay) + (px(x0, y1) * (1 - ax) + px(x1, y1) * ax) * ay;
          o[c] = (unsigned char)std::lround(std::max(0.0, std::min(255.0, v)));
        }
      }
    }
  }
  return out;
}

int keep_aspect(int ow, int oh, int side, bool is_width) {
  if (ow <= 0 || oh <= 0) return side;
  return std::max(1, (int)std::lround(is_width ? (double)side * oh / ow : (double)side * ow / oh));
}

void view_rect_to_image(int x0, int y0, int x1, int y1, double ox, double oy, double zoom, int iw, int ih, int &rx,
                        int &ry, int &rw, int &rh) {
  if (zoom <= 0) zoom = 1;
  double ax = (std::min(x0, x1) - ox) / zoom, ay = (std::min(y0, y1) - oy) / zoom;
  double bx = (std::max(x0, x1) - ox) / zoom, by = (std::max(y0, y1) - oy) / zoom;
  int l = std::max(0, std::min(iw, (int)std::floor(ax))), t = std::max(0, std::min(ih, (int)std::floor(ay)));
  int r = std::max(0, std::min(iw, (int)std::ceil(bx))), b = std::max(0, std::min(ih, (int)std::ceil(by)));
  rx = l;
  ry = t;
  rw = r - l;
  rh = b - t;
}

} // namespace view
