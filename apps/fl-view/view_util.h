// fl-view: reading EXIF data and editing pixels. No FLTK here (unit tested).
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace view {

// ---- EXIF (JPEG APP1, or a TIFF header) ---------------------------------------

struct Exif {
  bool found = false;
  int orientation = 1; // 1..8 as in the tag; 1 = upright
  std::string make, model, lens, software, taken; // taken: "2026:11:22 14:03:07"
  double exposure = 0;                            // seconds
  double fnumber = 0, focal = 0;                  // f/, mm
  int iso = 0, focal35 = 0, width = 0, height = 0;
  int flash = -1; // -1 unknown, else the tag's bits (bit 0: fired)
  bool has_gps = false;
  double lat = 0, lon = 0, alt = 0;
};

Exif parse_exif(const unsigned char *data, size_t len);  // the whole file or the APP1 payload
Exif read_exif(const std::string &path);                 // reads the start of a file

// readable rows for an information window: {"Camera", "Canon EOS R6"}, ...
std::vector<std::pair<std::string, std::string>> exif_rows(const Exif &e);
std::string exposure_text(double seconds);              // "1/250 s", "2 s"

// rotation (clockwise degrees) and mirroring that make an image upright
void orientation_transform(int orientation, int &rotate, bool &flip);

// ---- pixels: d bytes per pixel (1..4), rows of w*d bytes ---------------------

struct Pixels {
  int w = 0, h = 0, d = 3;
  std::vector<unsigned char> data;
};

Pixels crop(const Pixels &p, int x, int y, int w, int h); // clamped to the image
Pixels resize(const Pixels &p, int w, int h);              // area averaging down, bilinear up
// the other side for a new width or height that keeps the aspect ratio
int keep_aspect(int old_w, int old_h, int new_side, bool new_is_width);

// a crop rectangle from two corners in view coordinates, given where the image is
// drawn (ox, oy) and its zoom; in image pixels, clamped
void view_rect_to_image(int x0, int y0, int x1, int y1, double ox, double oy, double zoom, int img_w, int img_h,
                        int &rx, int &ry, int &rw, int &rh);

} // namespace view
