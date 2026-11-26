#include <cmath>
// fl-paint: brushes, shapes, fill, crop, scale, layers, colours
#include "paint_util.h"
#include "test.h"

using namespace paint;

int main() {
  Color red = rgba(255, 0, 0), white = rgba(255, 255, 255);
  CHECK_EQ(ch_r(red), 255);
  CHECK_EQ(ch_a(red), 255);
  CHECK(over(red, white) == red);
  Color half = rgba(255, 0, 0, 128);
  Color m = over(half, white);
  CHECK(ch_r(m) == 255 && ch_g(m) > 120 && ch_g(m) < 135 && ch_a(m) == 255);
  CHECK(over(0, white) == white);
  CHECK(over(half, 0) == half);

  Rect r = span(10, 8, 2, 3, 100, 100);
  CHECK(r.x == 2 && r.y == 3 && r.w == 9 && r.h == 6);
  CHECK(span(-5, -5, 3, 3, 2, 2).w == 2);

  Canvas c(40, 30, white);
  stroke(c, 5, 15, 35, 15, red, 4);
  CHECK(c.get(20, 15) == red);      // on the line
  CHECK(c.get(20, 25) == white);    // far from it
  CHECK(c.get(2, 15) == white);     // before its start (round end, radius 2)
  stroke(c, 20, 15, 20, 15, 0, 6, true); // erase a dot
  CHECK_EQ(ch_a(c.get(20, 15)), 0);

  Canvas s(20, 20, white);
  rectangle(s, {2, 2, 10, 10}, red, 1, false);
  CHECK(s.get(2, 2) == red);
  CHECK(s.get(11, 11) == red);
  CHECK(s.get(6, 6) == white); // outline only
  rectangle(s, {2, 2, 10, 10}, red, 1, true);
  CHECK(s.get(6, 6) == red);
  Canvas e(21, 21, white);
  ellipse(e, {0, 0, 21, 21}, red, 2, true);
  CHECK(e.get(10, 10) == red);
  CHECK(e.get(0, 0) == white); // the corner is outside the circle
  Canvas e2(21, 21, white);
  ellipse(e2, {0, 0, 21, 21}, red, 2, false);
  CHECK(e2.get(10, 10) == white); // a ring
  CHECK(e2.get(10, 0) != white);

  // fill stops at the outline
  Canvas f(20, 20, white);
  rectangle(f, {5, 5, 10, 10}, red, 1, false);
  Color blue = rgba(0, 0, 255);
  flood_fill(f, 9, 9, blue);
  CHECK(f.get(9, 9) == blue);
  CHECK(f.get(5, 5) == red);
  CHECK(f.get(1, 1) == white);
  flood_fill(f, 1, 1, blue);
  CHECK(f.get(19, 19) == blue);
  CHECK(f.get(5, 9) == red);

  Canvas big(10, 6, white);
  big.set(7, 4, red);
  Canvas cr = crop(big, {5, 2, 4, 4});
  CHECK(cr.w == 4 && cr.h == 4);
  CHECK(cr.get(2, 2) == red);
  Canvas rc = resize_canvas(big, 12, 8, 1, 1, blue);
  CHECK(rc.get(0, 0) == blue);
  CHECK(rc.get(8, 5) == red);
  Canvas sc = scale(Canvas(4, 4, red), 8, 2);
  CHECK(sc.w == 8 && sc.h == 2 && sc.get(5, 1) == red);
  Canvas fl = flip(big, true);
  CHECK(fl.get(2, 4) == red);
  Canvas fv = flip(big, false);
  CHECK(fv.get(7, 1) == red);
  Canvas rt = rotate90(big, true);
  CHECK(rt.w == 6 && rt.h == 10);
  CHECK(rt.get(1, 7) == red);
  Canvas rl = rotate90(big, false);
  CHECK(rl.get(4, 2) == red);

  std::vector<Layer> layers(2);
  layers[0].c = Canvas(4, 4, white);
  layers[1].c = Canvas(4, 4, 0);
  layers[1].c.set(1, 1, red);
  CHECK(flatten(layers, 0).get(1, 1) == red);
  layers[1].visible = false;
  CHECK(flatten(layers, 0).get(1, 1) == white);
  layers[1].visible = true;
  layers[1].opacity = 0.5;
  Color fm = flatten(layers, 0).get(1, 1);
  CHECK(ch_g(fm) > 120 && ch_g(fm) < 135);

  CHECK_EQ(to_hex(rgba(0x12, 0xab, 0xff)), std::string("#12abff"));
  CHECK_EQ(to_hex(rgba(1, 2, 3, 4)), std::string("#01020304"));
  Color h;
  CHECK(from_hex("#12abff", h) && h == rgba(0x12, 0xab, 0xff));
  CHECK(from_hex("#01020304", h) && h == rgba(1, 2, 3, 4));
  CHECK(!from_hex("12abff", h));
  CHECK(!from_hex("#12abfz", h));
  // pen pressure: a fifth of the size at a light touch, all of it pressed hard
  CHECK(std::fabs(paint::pressure_size(10, 0) - 2) < 1e-9);
  CHECK(std::fabs(paint::pressure_size(10, 1) - 10) < 1e-9);
  CHECK(std::fabs(paint::pressure_size(10, 0.5) - 6) < 1e-9);
  CHECK(paint::pressure_size(10, 5) == 10);      // out of range: clamped
  CHECK(paint::pressure_size(10, -1) == 2);
  CHECK(paint::pressure_size(2, 0) == 1);         // never under a pixel
  CHECK(paint::pressure_size(10, std::nan("")) == 2);
  return test_result("paint");
}
