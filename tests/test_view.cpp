// fl-view: EXIF and pixel editing
#include "test.h"
#include "view_util.h"

#include <cstring>

using namespace view;

// a little-endian TIFF block with IFD0 (Make, Model, Orientation, ExifIFD, GPS),
// an Exif IFD and a GPS IFD, in a JPEG APP1 segment
static std::vector<unsigned char> sample_jpeg() {
  std::vector<unsigned char> t;
  auto u16 = [&](unsigned v) { t.push_back(v & 255); t.push_back(v >> 8); };
  auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; i++) t.push_back((v >> (8 * i)) & 255); };
  auto at16 = [&](size_t o, unsigned v) { t[o] = v & 255; t[o + 1] = v >> 8; };
  auto at32 = [&](size_t o, uint32_t v) { for (int i = 0; i < 4; i++) t[o + i] = (v >> (8 * i)) & 255; };
  t = {'I', 'I', 42, 0};
  u32(8);
  // IFD0: 5 entries
  size_t ifd0 = t.size();
  u16(5);
  size_t e = t.size();
  for (int i = 0; i < 5 * 12; i++) t.push_back(0);
  u32(0);
  auto entry = [&](size_t o, unsigned tag, unsigned type, uint32_t count, uint32_t val) {
    at16(o, tag); at16(o + 2, type); at32(o + 4, count); at32(o + 8, val);
  };
  size_t make = t.size();
  for (char c : std::string("Canon\0", 6)) t.push_back((unsigned char)c);
  size_t model = t.size();
  for (char c : std::string("Canon EOS R6\0", 13)) t.push_back((unsigned char)c);
  // Exif IFD: ExposureTime, FNumber, ISO, DateTimeOriginal, FocalLength
  size_t exif = t.size();
  u16(5);
  size_t ee = t.size();
  for (int i = 0; i < 5 * 12; i++) t.push_back(0);
  u32(0);
  size_t rationals = t.size();
  u32(1); u32(250);   // 1/250
  u32(28); u32(10);   // f/2.8
  u32(50); u32(1);    // 50 mm
  size_t date = t.size();
  for (char c : std::string("2026:11:22 14:03:07\0", 20)) t.push_back((unsigned char)c);
  entry(ee, 0x829A, 5, 1, (uint32_t)rationals);
  entry(ee + 12, 0x829D, 5, 1, (uint32_t)rationals + 8);
  entry(ee + 24, 0x8827, 3, 1, 400);
  entry(ee + 36, 0x9003, 2, 20, (uint32_t)date);
  entry(ee + 48, 0x920A, 5, 1, (uint32_t)rationals + 16);
  // GPS IFD: S 33°51'54", E 151°12'36"
  size_t gps = t.size();
  u16(4);
  size_t ge = t.size();
  for (int i = 0; i < 4 * 12; i++) t.push_back(0);
  u32(0);
  size_t lat = t.size();
  u32(33); u32(1); u32(51); u32(1); u32(54); u32(1);
  size_t lon = t.size();
  u32(151); u32(1); u32(12); u32(1); u32(36); u32(1);
  entry(ge, 1, 2, 2, 'S');
  entry(ge + 12, 2, 5, 3, (uint32_t)lat);
  entry(ge + 24, 3, 2, 2, 'E');
  entry(ge + 36, 4, 5, 3, (uint32_t)lon);
  entry(e, 0x010F, 2, 6, (uint32_t)make);
  entry(e + 12, 0x0110, 2, 13, (uint32_t)model);
  entry(e + 24, 0x0112, 3, 1, 6);
  entry(e + 36, 0x8769, 4, 1, (uint32_t)exif);
  entry(e + 48, 0x8825, 4, 1, (uint32_t)gps);
  (void)ifd0;
  std::vector<unsigned char> j = {0xFF, 0xD8, 0xFF, 0xE0, 0, 4, 0, 0}; // SOI, a tiny APP0
  size_t len = t.size() + 8;
  j.insert(j.end(), {0xFF, 0xE1, (unsigned char)(len >> 8), (unsigned char)(len & 255), 'E', 'x', 'i', 'f', 0, 0});
  j.insert(j.end(), t.begin(), t.end());
  j.insert(j.end(), {0xFF, 0xD9});
  return j;
}

int main() {
  auto jpg = sample_jpeg();
  Exif x = parse_exif(jpg.data(), jpg.size());
  CHECK(x.found);
  CHECK_EQ(x.make, std::string("Canon"));
  CHECK_EQ(x.model, std::string("Canon EOS R6"));
  CHECK_EQ(x.orientation, 6);
  CHECK_NEAR(x.exposure, 0.004);
  CHECK_NEAR(x.fnumber, 2.8);
  CHECK_EQ(x.iso, 400);
  CHECK_NEAR(x.focal, 50.0);
  CHECK_EQ(x.taken, std::string("2026:11:22 14:03:07"));
  CHECK(x.has_gps);
  CHECK(x.lat < -33.86 && x.lat > -33.87);
  CHECK(x.lon > 151.20 && x.lon < 151.22);
  auto rows = exif_rows(x);
  CHECK_EQ(rows[0].second, std::string("Canon EOS R6")); // the make is not said twice
  bool exp = false, loc = false;
  for (auto &r : rows) {
    if (r.first == "Exposure") exp = r.second == "1/250 s  f/2.8  ISO 400";
    if (r.first == "Location") loc = r.second.find("33.86500°S") != std::string::npos;
    if (r.first == "Taken") CHECK_EQ(r.second, std::string("2026-11-22 14:03:07"));
  }
  CHECK(exp);
  CHECK(loc);
  CHECK_EQ(exposure_text(2.0), std::string("2 s"));
  CHECK_EQ(exposure_text(1.0 / 60), std::string("1/60 s"));
  // broken or missing data
  CHECK(!parse_exif(jpg.data(), 20).found);
  unsigned char png[] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
  CHECK(!parse_exif(png, sizeof png).found);
  std::vector<unsigned char> cut(jpg.begin(), jpg.begin() + 60);
  parse_exif(cut.data(), cut.size()); // must not crash
  CHECK(exif_rows(Exif{}).empty());

  int rot;
  bool flip;
  orientation_transform(6, rot, flip);
  CHECK(rot == 90 && !flip);
  orientation_transform(5, rot, flip);
  CHECK(rot == 270 && flip);
  orientation_transform(4, rot, flip);
  CHECK(rot == 180 && flip);
  orientation_transform(99, rot, flip);
  CHECK(rot == 0 && !flip);

  // pixels: a 4x2 gray ramp
  Pixels p;
  p.w = 4;
  p.h = 2;
  p.d = 1;
  p.data = {0, 10, 20, 30, 40, 50, 60, 70};
  Pixels c = crop(p, 1, 0, 2, 2);
  CHECK(c.w == 2 && c.h == 2);
  CHECK(c.data == std::vector<unsigned char>({10, 20, 50, 60}));
  Pixels c2 = crop(p, 3, 1, 10, 10); // clamped
  CHECK(c2.w == 1 && c2.h == 1 && c2.data[0] == 70);
  Pixels half = resize(p, 2, 1);
  CHECK(half.data == std::vector<unsigned char>({25, 45}));
  Pixels up = resize(p, 8, 4);
  CHECK(up.w == 8 && up.h == 4);
  CHECK_EQ((int)up.data[0], 0);
  CHECK_EQ((int)up.data[8 * 4 - 1], 70);
  Pixels rgb;
  rgb.w = rgb.h = 2;
  rgb.d = 3;
  rgb.data = {255, 0, 0, 255, 0, 0, 255, 0, 0, 255, 0, 0};
  Pixels one = resize(rgb, 1, 1);
  CHECK(one.data == std::vector<unsigned char>({255, 0, 0}));
  CHECK_EQ(keep_aspect(4000, 3000, 800, true), 600);
  CHECK_EQ(keep_aspect(4000, 3000, 600, false), 800);

  int rx, ry, rw, rh;
  // image drawn at (100, 50) at half size: view (110, 60)-(210, 160) -> image (20, 20) 200x200
  view_rect_to_image(210, 160, 110, 60, 100, 50, 0.5, 1000, 1000, rx, ry, rw, rh);
  CHECK(rx == 20 && ry == 20 && rw == 200 && rh == 200);
  view_rect_to_image(0, 0, 5000, 5000, 100, 50, 0.5, 1000, 800, rx, ry, rw, rh);
  CHECK(rx == 0 && ry == 0 && rw == 1000 && rh == 800);
  return test_result("view");
}
