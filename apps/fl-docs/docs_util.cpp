// fl-docs helpers (see docs_util.h)
#include "docs_util.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

} // namespace docs
