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

} // namespace web
