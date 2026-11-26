// fl-snip helpers
#include <cstdlib>
#include <string>

#include "snip_util.h"
#include "test.h"

using namespace snip;

static void test_rects() {
  CHECK(span(10, 20, 4, 8) == (Rect{4, 8, 7, 13}));
  CHECK(span(3, 3, 3, 3) == (Rect{3, 3, 1, 1}));
  CHECK(clip({-5, -5, 20, 20}, 10, 8) == (Rect{0, 0, 10, 8}));
  CHECK(clip({12, 2, 5, 5}, 10, 8).empty());
  // 2x output scale: whole pixels, outwards
  CHECK(scale({1, 2, 3, 4}, 2, 2) == (Rect{2, 4, 6, 8}));
  CHECK(scale({1, 1, 1, 1}, 1.5, 1.5) == (Rect{1, 1, 2, 2}));
}

static void test_windows() {
  auto w = parse_windows("10 20 300 200 Text Editor\n0 0 1280 720 Files\nbad line\n5 5 0 10 empty\n");
  CHECK_EQ(w.size(), 2u);
  CHECK_EQ(w[0].title, std::string("Text Editor"));
  CHECK(w[0].r == (Rect{10, 20, 300, 200}));
  CHECK_EQ(window_at(w, 50, 50), 0);   // the topmost of the two
  CHECK_EQ(window_at(w, 900, 600), 1);
  CHECK_EQ(window_at(w, 2000, 10), -1);
}

static void test_polygon() {
  std::vector<Point> tri = {{0, 0}, {10, 0}, {0, 10}};
  CHECK(inside_polygon(tri, 2, 2));
  CHECK(!inside_polygon(tri, 8, 8));
  CHECK(bounds(tri) == (Rect{0, 0, 11, 11}));
}

static void test_images() {
  std::string ppm = "P6\n# grim\n3 2\n255\n";
  for (int i = 0; i < 6; i++) ppm += std::string{(char)(i * 10), (char)(i * 10 + 1), (char)(i * 10 + 2)};
  Image img;
  CHECK(parse_ppm(ppm, img));
  CHECK_EQ(img.w, 3);
  CHECK_EQ(img.h, 2);
  CHECK_EQ((int)img.at(2, 1)[0], 50);
  CHECK_EQ((int)img.at(2, 1)[3], 255);
  CHECK(!parse_ppm("P6\n3 2\n255\nshort", img));
  CHECK(!parse_ppm("P3\n1 1\n255\n0 0 0", img));

  Image c = crop(img, {1, 0, 5, 5});
  CHECK_EQ(c.w, 2);
  CHECK_EQ(c.h, 2);
  CHECK_EQ((int)c.at(0, 0)[0], 10);

  // freeform: outside the triangle is transparent
  Image sq;
  sq.w = sq.h = 10;
  sq.px.assign(10 * 10 * 4, 200);
  mask_outside(sq, {{0, 0}, {10, 0}, {0, 10}});
  CHECK_EQ((int)sq.at(1, 1)[3], 200);
  CHECK_EQ((int)sq.at(9, 9)[3], 0);

  // pixelate averages each block
  Image px;
  px.w = 2;
  px.h = 1;
  px.px = {0, 0, 0, 255, 100, 100, 100, 255};
  pixelate(px, {0, 0, 2, 1}, 2);
  CHECK_EQ((int)px.at(0, 0)[0], 50);
  CHECK_EQ((int)px.at(1, 0)[0], 50);
}

static void test_names() {
  setenv("TZ", "UTC", 1);
  CHECK_EQ(file_name(0), std::string("Screenshot-1970-01-01-000000.png"));
}

int main() {
  test_rects();
  test_windows();
  test_polygon();
  test_images();
  test_names();
  // screen recording
  {
    std::string p = record_pipeline(7, 42, false);
    CHECK(p.find("pipewiresrc fd=7 path=42") == 0);
    CHECK(p.find("framerate=30/1") != std::string::npos);
    CHECK(p.find("webmmux name=mux") != std::string::npos);
    CHECK(p.find("filesink name=sink") != std::string::npos);
    CHECK(p.find("pulsesrc") == std::string::npos);
    std::string q = record_pipeline(-1, 42, true);
    CHECK(q.find("fd=") == std::string::npos);
    CHECK(q.find("pulsesrc device=@DEFAULT_MONITOR@") != std::string::npos);
    CHECK(q.find("! mux.") != std::string::npos);
    time_t t = 1700000000;
    std::string n = recording_name(t);
    CHECK(n.rfind("Screencast-", 0) == 0);
    CHECK(n.size() > 5 && n.substr(n.size() - 5) == ".webm");
  }
  // the colour picker and the ruler
  CHECK_EQ(snip::hex_color(0x35, 0x84, 0xe4), std::string("#3584e4"));
  CHECK_EQ(snip::hex_color(0, 0, 0), std::string("#000000"));
  CHECK_EQ(snip::ruler_label(10, 10, 130, 55), std::string("120 \xc3\x97 45 px, 128.2 px"));
  CHECK_EQ(snip::ruler_label(5, 5, 5, 5), std::string("0 \xc3\x97 0 px, 0.0 px"));
  return test_result("snip");
}
