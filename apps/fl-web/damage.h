// fl-web: the parts of a frame that changed
#pragma once
#include <vector>

namespace web {

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
};

// The parts of a width x height frame that n damage rectangles cover:
// clipped to the frame, empty ones dropped, merged into their bounding box
// when there are more than `max`. `full` is set when they are the whole
// frame, or when there are no rectangles at all (damage not known); the
// result is empty when nothing changed.
std::vector<Rect> clip_damage(const Rect *rects, unsigned n, int width, int height, bool &full, unsigned max = 8);

// The parts that differ between two frames of the same size in 4-byte
// pixels, `stride` bytes a row: bands of changed rows, each as wide as its
// changes, at most `max` (neighbours merged). Empty when they are the same;
// `full` when the whole frame changed.
std::vector<Rect> diff_damage(const unsigned char *a, const unsigned char *b, int width, int height, int stride,
                              bool &full, unsigned max = 8);

} // namespace web
