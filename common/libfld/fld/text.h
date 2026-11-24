// Fitting text into a width: ellipsis and label wrapping, and the colour of
// text on a coloured background
#pragma once
#include <FL/Enumerations.H>
#include <functional>
#include <string>
#include <vector>

namespace fld {

// Measures a string in pixels; the default is fl_width() with the current font.
using TextMeasure = std::function<double(const std::string &)>;

// `s` if it fits in `width`, otherwise as much of it as fits followed by "…"
// (cut at a character boundary).
std::string ellipsize(const std::string &s, int width, const TextMeasure &measure = nullptr);

// Breaks `s` into at most `max_lines` lines of `width`. Lines break after a
// space, '-', '_' or '.' when there is one on the line, else between
// characters; the last line ends with "…" when the text does not fit.
std::vector<std::string> wrap_label(const std::string &s, int width, int max_lines,
                                    const TextMeasure &measure = nullptr);

// WCAG 2 contrast ratio of two colours (1 to 21).
double contrast_ratio(Fl_Color a, Fl_Color b);
// The text colour for a background: near-black or white, whichever reads
// better on it.
Fl_Color text_on(Fl_Color bg);

} // namespace fld
