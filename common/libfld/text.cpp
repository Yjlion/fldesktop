#include "fld/text.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>
#include <cmath>

namespace fld {

namespace {

const char kEllipsis[] = "…";

double measure_with(const TextMeasure &m, const std::string &s) {
  if (m) return m(s);
  return fl_width(s.c_str(), (int)s.size());
}

size_t char_len(const std::string &s, size_t i) {
  int n = fl_utf8len1(s[i]);
  if (n < 1) n = 1;
  if (i + n > s.size()) n = (int)(s.size() - i);
  return (size_t)n;
}

// drops the last character (not a byte) of s
void pop_char(std::string &s) {
  while (!s.empty()) {
    unsigned char c = (unsigned char)s.back();
    s.pop_back();
    if ((c & 0xC0) != 0x80) break;
  }
}

bool is_break(char c) { return c == ' ' || c == '-' || c == '_' || c == '.'; }

} // namespace

std::string ellipsize(const std::string &s, int width, const TextMeasure &measure) {
  if (measure_with(measure, s) <= width) return s;
  std::string out = s;
  while (!out.empty() && measure_with(measure, out + kEllipsis) > width) pop_char(out);
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out + kEllipsis;
}

std::vector<std::string> wrap_label(const std::string &s, int width, int max_lines,
                                    const TextMeasure &measure) {
  std::vector<std::string> lines;
  if (max_lines < 1) return lines;
  std::string cur;
  size_t i = 0;
  while (i < s.size()) {
    size_t len = char_len(s, i);
    std::string ch = s.substr(i, len);
    if (!cur.empty() && measure_with(measure, cur + ch) > width) {
      if ((int)lines.size() + 1 == max_lines) break; // the last line: ellipsis below
      // break after the last separator on the line, if it leaves something
      size_t b = std::string::npos; // the line ends before b
      for (size_t k = cur.size(); k > 1; k--)
        if (is_break(cur[k - 1])) {
          b = k;
          break;
        }
      std::string rest;
      if (b != std::string::npos) {
        rest = cur.substr(b);
        cur.erase(b);
        while (!cur.empty() && cur.back() == ' ') cur.pop_back();
        size_t lead = rest.find_first_not_of(' ');
        rest = lead == std::string::npos ? "" : rest.substr(lead);
      }
      lines.push_back(cur);
      cur = rest;
    }
    cur += ch;
    i += len;
  }
  if (i < s.size()) {
    // the rest of the text does not fit: the last line gets an ellipsis
    cur += s.substr(i);
    cur = ellipsize(cur, width, measure);
  }
  if (!cur.empty()) lines.push_back(cur);
  return lines;
}

static double luminance(Fl_Color c) {
  unsigned char rgb[3];
  Fl::get_color(c, rgb[0], rgb[1], rgb[2]);
  double l[3];
  for (int k = 0; k < 3; k++) {
    double v = rgb[k] / 255.0;
    l[k] = v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  }
  return 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2];
}

double contrast_ratio(Fl_Color a, Fl_Color b) {
  double la = luminance(a), lb = luminance(b);
  if (la < lb) std::swap(la, lb);
  return (la + 0.05) / (lb + 0.05);
}

Fl_Color text_on(Fl_Color bg) {
  Fl_Color dark = fl_rgb_color(0x14, 0x14, 0x16);
  return contrast_ratio(bg, dark) >= contrast_ratio(bg, FL_WHITE) ? dark : FL_WHITE;
}

} // namespace fld
