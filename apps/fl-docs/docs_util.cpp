// fl-docs helpers (see docs_util.h)
#include "docs_util.h"

#include <algorithm>
#include <cctype>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdlib>
#include <sstream>

namespace docs {

Layout layout(const std::vector<PageSize> &pages, double zoom, int min_width) {
  Layout l;
  l.zoom = zoom;
  double s = l.scale();
  int maxw = 0;
  for (const PageSize &p : pages) maxw = std::max(maxw, (int)std::lround(p.w * s));
  l.width = std::max(min_width, maxw + 2 * kGap);
  int y = kGap;
  for (const PageSize &p : pages) {
    int w = std::max(1, (int)std::lround(p.w * s)), h = std::max(1, (int)std::lround(p.h * s));
    l.x.push_back((l.width - w) / 2);
    l.y.push_back(y);
    l.w.push_back(w);
    l.h.push_back(h);
    y += h + kGap;
  }
  l.height = y;
  return l;
}

int page_at(const Layout &l, int y) {
  if (l.y.empty()) return -1;
  // the first page whose bottom (plus half the gap) is below y
  for (size_t i = 0; i < l.y.size(); i++)
    if (y < l.y[i] + l.h[i] + kGap / 2) return (int)i;
  return (int)l.y.size() - 1;
}

int current_page(const Layout &l, int top, int height) {
  int best = page_at(l, top), best_px = -1;
  for (size_t i = 0; i < l.y.size(); i++) {
    int a = std::max(top, l.y[i]), b = std::min(top + height, l.y[i] + l.h[i]);
    if (b - a > best_px) {
      best_px = b - a;
      best = (int)i;
    }
    if (l.y[i] > top + height) break;
  }
  return best;
}

double fit_zoom(const std::vector<PageSize> &pages, int index, Fit fit, int view_w, int view_h) {
  if (pages.empty() || fit == Fit::None) return 1;
  index = std::max(0, std::min((int)pages.size() - 1, index));
  const PageSize &p = pages[index];
  double widest = 0;
  for (const PageSize &q : pages) widest = std::max(widest, q.w);
  double zw = (view_w - 2 * kGap) / ((fit == Fit::Width ? widest : p.w) * kPixelsPerPoint);
  double zh = (view_h - 2 * kGap) / (p.h * kPixelsPerPoint);
  double z = fit == Fit::Width ? zw : std::min(zw, zh);
  return std::max(0.1, std::min(8.0, z));
}

double zoom_step(double zoom, int dir) {
  static const double steps[] = {0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5,
                                 1.75, 2.0, 2.5, 3.0, 4.0, 5.0, 6.0, 8.0};
  const int n = sizeof steps / sizeof steps[0];
  if (dir > 0) {
    for (int i = 0; i < n; i++)
      if (steps[i] > zoom + 1e-6) return steps[i];
    return steps[n - 1];
  }
  for (int i = n - 1; i >= 0; i--)
    if (steps[i] < zoom - 1e-6) return steps[i];
  return steps[0];
}

Rect to_strip(const Layout &l, int page, double x, double y, double w, double h) {
  Rect r;
  if (page < 0 || page >= (int)l.x.size()) return r;
  double s = l.scale();
  r.x = l.x[page] + (int)std::floor(x * s);
  r.y = l.y[page] + (int)std::floor(y * s);
  r.w = std::max(1, (int)std::ceil(w * s));
  r.h = std::max(1, (int)std::ceil(h * s));
  return r;
}

std::string page_ranges(const std::string &text, int n, bool *ok) {
  *ok = true;
  std::string t;
  for (char c : text)
    if (!isspace((unsigned char)c)) t += (char)tolower((unsigned char)c);
  if (t.empty() || t == "all") return "";
  std::vector<std::pair<int, int>> ranges;
  std::stringstream ss(t);
  std::string part;
  while (std::getline(ss, part, ',')) {
    if (part.empty()) continue;
    size_t dash = part.find('-');
    auto num = [](const std::string &s, int *v) {
      if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 6) return false;
      *v = std::atoi(s.c_str());
      return true;
    };
    int a, b;
    if (dash == std::string::npos) {
      if (!num(part, &a)) {
        *ok = false;
        return "";
      }
      b = a;
    } else {
      std::string sa = part.substr(0, dash), sb = part.substr(dash + 1);
      if (sa.empty()) a = 1;
      else if (!num(sa, &a)) {
        *ok = false;
        return "";
      }
      if (sb.empty()) b = n;
      else if (!num(sb, &b)) {
        *ok = false;
        return "";
      }
    }
    if (a < 1 || b < a || a > n) {
      *ok = false;
      return "";
    }
    ranges.push_back({a, std::min(b, n)});
  }
  if (ranges.empty()) {
    *ok = false;
    return "";
  }
  std::string out;
  for (auto &r : ranges) {
    if (!out.empty()) out += ",";
    out += std::to_string(r.first);
    if (r.second != r.first) out += "-" + std::to_string(r.second);
  }
  return out;
}

namespace {

std::string lower_ext(const std::string &path) {
  size_t slash = path.find_last_of('/');
  size_t dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
  std::string e = path.substr(dot + 1);
  for (char &c : e) c = (char)tolower((unsigned char)c);
  return e;
}

unsigned be16(const std::string &d, size_t i) { return ((unsigned char)d[i] << 8) | (unsigned char)d[i + 1]; }
unsigned be32(const std::string &d, size_t i) { return (be16(d, i) << 16) | be16(d, i + 2); }
unsigned le16(const std::string &d, size_t i) { return (unsigned char)d[i] | ((unsigned char)d[i + 1] << 8); }
unsigned le32(const std::string &d, size_t i) { return le16(d, i) | (le16(d, i + 2) << 16); }

} // namespace

Format sniff(const std::string &head, const std::string &path) {
  std::string ext = lower_ext(path);
  if (head.compare(0, 5, "%PDF-") == 0) return Format::Pdf;
  if (head.compare(0, 4, "PK\x03\x04") == 0) {
    // ODF and EPUB store "mimetype" first and uncompressed
    if (head.size() > 38 && head.compare(30, 8, "mimetype") == 0 && le16(head, 8) == 0) {
      size_t size = le32(head, 18), start = 30 + le16(head, 26) + le16(head, 28);
      std::string mime = start < head.size() ? head.substr(start, std::min<size_t>(size, 100)) : "";
      while (!mime.empty() && isspace((unsigned char)mime.back())) mime.pop_back();
      if (mime == "application/epub+zip") return Format::Epub;
      if (mime == "application/vnd.oasis.opendocument.text" ||
          mime == "application/vnd.oasis.opendocument.text-template")
        return Format::Odt;
      if (mime.compare(0, 35, "application/vnd.oasis.opendocument.") == 0) return Format::OdfOther;
    }
    return Format::Zip;
  }
  if (head.compare(0, 6, "Rar!\x1a\x07") == 0) return Format::Archive;
  if (head.compare(0, 6, std::string("7z\xbc\xaf\x27\x1c", 6)) == 0) return Format::Archive;
  if (head.size() > 262 && head.compare(257, 5, "ustar") == 0) return Format::Archive;
  // PDF allows a little junk before its header
  if (head.find("%PDF-") != std::string::npos && head.find("%PDF-") < 1024) return Format::Pdf;
  if (ext == "pdf") return Format::Pdf; // let poppler say what is wrong
  if (is_markdown_name(path)) return Format::Markdown;
  return Format::Unknown;
}

bool natural_less(const std::string &a, const std::string &b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[j];
    if (isdigit(ca) && isdigit(cb)) {
      size_t i2 = i, j2 = j;
      while (i2 < a.size() && a[i2] == '0') i2++;
      while (j2 < b.size() && b[j2] == '0') j2++;
      size_t ie = i2, je = j2;
      while (ie < a.size() && isdigit((unsigned char)a[ie])) ie++;
      while (je < b.size() && isdigit((unsigned char)b[je])) je++;
      if (ie - i2 != je - j2) return ie - i2 < je - j2;
      int c = a.compare(i2, ie - i2, b, j2, je - j2);
      if (c) return c < 0;
      i = ie;
      j = je;
      continue;
    }
    int la = tolower(ca), lb = tolower(cb);
    if (la != lb) return la < lb;
    i++;
    j++;
  }
  if ((a.size() - i) != (b.size() - j)) return a.size() - i < b.size() - j;
  return a < b;
}

bool image_size(const std::string &d, int &w, int &h) {
  w = h = 0;
  if (d.size() > 24 && d.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) {
    w = (int)be32(d, 16);
    h = (int)be32(d, 20);
  } else if (d.size() > 10 && (d.compare(0, 6, "GIF87a") == 0 || d.compare(0, 6, "GIF89a") == 0)) {
    w = (int)le16(d, 6);
    h = (int)le16(d, 8);
  } else if (d.size() > 26 && d.compare(0, 2, "BM") == 0) {
    w = (int)le32(d, 18);
    h = std::abs((int)le32(d, 22));
  } else if (d.size() > 4 && (unsigned char)d[0] == 0xff && (unsigned char)d[1] == 0xd8) {
    size_t i = 2;
    while (i + 9 < d.size()) {
      if ((unsigned char)d[i] != 0xff) { i++; continue; }
      unsigned char m = (unsigned char)d[i + 1];
      if (m == 0xff) { i++; continue; }
      if (m == 0xd8 || m == 0x01 || (m >= 0xd0 && m <= 0xd7)) { i += 2; continue; }
      unsigned len = be16(d, i + 2);
      // start of frame (not DHT 0xc4, JPG 0xc8, DAC 0xcc)
      if (m >= 0xc0 && m <= 0xcf && m != 0xc4 && m != 0xc8 && m != 0xcc) {
        h = (int)be16(d, i + 5);
        w = (int)be16(d, i + 7);
        break;
      }
      i += 2 + len;
    }
  }
  return w > 0 && h > 0 && w < 100000 && h < 100000;
}

bool is_image_name(const std::string &name) {
  std::string e = lower_ext(name);
  return e == "png" || e == "jpg" || e == "jpeg" || e == "gif" || e == "bmp" || e == "svg";
}

double parse_length(const std::string &text, double fallback) {
  const char *s = text.c_str();
  char *end = nullptr;
  double v = strtod(s, &end);
  if (end == s) return fallback;
  std::string unit(end);
  while (!unit.empty() && isspace((unsigned char)unit.back())) unit.pop_back();
  for (char &c : unit) c = (char)tolower((unsigned char)c);
  if (unit == "pt") return v;
  if (unit == "in") return v * 72;
  if (unit == "cm") return v * 72 / 2.54;
  if (unit == "mm") return v * 72 / 25.4;
  if (unit == "pc") return v * 12;
  if (unit == "px") return v * 0.75;
  return fallback;
}

} // namespace docs

