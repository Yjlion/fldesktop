#include "fld/avatar.h"
#include "fld/accents.h"

#include <FL/Fl_Image.H>
#include <FL/Fl_Pixmap.H>
#include <FL/Fl_SVG_Image.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>
#include <algorithm>
#include <cairo.h>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>

namespace fld {

std::string avatar_color_hex(const std::string &user) {
  // FNV-1a: stable across runs and machines
  uint32_t h = 2166136261u;
  for (unsigned char c : user) h = (h ^ c) * 16777619u;
  const auto &p = accent_presets();
  return p[h % p.size()].hex;
}

Fl_Color avatar_color(const std::string &user) {
  unsigned long v = strtoul(avatar_color_hex(user).c_str() + 1, nullptr, 16);
  return fl_rgb_color((uchar)(v >> 16), (uchar)(v >> 8), (uchar)v);
}

std::string avatar_initial(const std::string &user, const std::string &real_name) {
  const std::string &s = real_name.empty() ? user : real_name;
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i >= s.size()) return "?";
  int len = 0;
  unsigned cp = fl_utf8decode(s.c_str() + i, s.c_str() + s.size(), &len);
  char buf[8];
  int n = fl_utf8encode((unsigned)fl_toupper(cp), buf);
  return std::string(buf, n);
}

void draw_initial_avatar(int x, int y, int size, const std::string &user, const std::string &real_name) {
  fl_color(avatar_color(user));
  fl_pie(x, y, size, size, 0, 360);
  fl_color(FL_WHITE);
  fl_font(FL_HELVETICA_BOLD, std::max(8, size / 2));
  fl_draw(avatar_initial(user, real_name).c_str(), x, y, size, size, FL_ALIGN_CENTER);
}

namespace {

// img scaled (bilinear) so that it covers size x size; RGB pixels to read
std::unique_ptr<Fl_RGB_Image> cover(Fl_Image *img, int size) {
  if (!img || img->w() <= 0 || img->h() <= 0 || size <= 0) return nullptr;
  std::unique_ptr<Fl_RGB_Image> pix;
  Fl_Image *src = img;
  if (auto *xpm = dynamic_cast<Fl_Pixmap *>(img)) {
    pix.reset(new Fl_RGB_Image(xpm));
    src = pix.get();
  }
  double s = std::max((double)size / src->w(), (double)size / src->h());
  int sw = std::max(size, (int)std::lround(src->w() * s));
  int sh = std::max(size, (int)std::lround(src->h() * s));
  Fl_RGB_Scaling old = Fl_Image::RGB_scaling();
  Fl_Image::RGB_scaling(FL_RGB_SCALING_BILINEAR);
  Fl_Image *c = src->copy(sw, sh);
  Fl_Image::RGB_scaling(old);
  auto *rgb = dynamic_cast<Fl_RGB_Image *>(c);
  if (!rgb) {
    delete c;
    return nullptr;
  }
  if (auto *svg = dynamic_cast<Fl_SVG_Image *>(rgb)) svg->normalize();
  if (!rgb->data() || !rgb->data()[0] || rgb->d() < 1 || rgb->d() > 4 || rgb->w() < size || rgb->h() < size) {
    delete rgb;
    return nullptr;
  }
  return std::unique_ptr<Fl_RGB_Image>(rgb);
}

// the center size x size square of `rgb` as straight RGBA
void crop_rgba(const Fl_RGB_Image *rgb, int size, uchar *out) {
  int d = rgb->d(), ld = rgb->ld() ? rgb->ld() : rgb->w() * d;
  int x0 = (rgb->w() - size) / 2, y0 = (rgb->h() - size) / 2;
  const uchar *data = (const uchar *)rgb->data()[0];
  for (int y = 0; y < size; y++) {
    const uchar *p = data + (size_t)(y0 + y) * ld + (size_t)x0 * d;
    uchar *o = out + (size_t)y * size * 4;
    for (int x = 0; x < size; x++, p += d, o += 4) {
      if (d < 3) o[0] = o[1] = o[2] = p[0];
      else o[0] = p[0], o[1] = p[1], o[2] = p[2];
      o[3] = d == 2 ? p[1] : d == 4 ? p[3] : 255;
    }
  }
}

// coverage of pixel (x, y) by the circle inscribed in size x size
double circle_alpha(int x, int y, int size) {
  double r = size / 2.0, dx = x + 0.5 - r, dy = y + 0.5 - r;
  return std::min(1.0, std::max(0.0, r - std::sqrt(dx * dx + dy * dy) + 0.5));
}

cairo_status_t append_png(void *closure, const unsigned char *data, unsigned int len) {
  static_cast<std::string *>(closure)->append((const char *)data, len);
  return CAIRO_STATUS_SUCCESS;
}

bool surface_png(cairo_surface_t *s, std::string &png) {
  png.clear();
  cairo_surface_flush(s);
  bool ok = cairo_surface_write_to_png_stream(s, append_png, &png) == CAIRO_STATUS_SUCCESS;
  cairo_surface_destroy(s);
  return ok && !png.empty();
}

} // namespace

Fl_RGB_Image *avatar_image(Fl_Image *img, int size, bool round) {
  auto rgb = cover(img, size);
  if (!rgb) return nullptr;
  uchar *buf = new uchar[(size_t)size * size * 4];
  crop_rgba(rgb.get(), size, buf);
  if (round)
    for (int y = 0; y < size; y++)
      for (int x = 0; x < size; x++) {
        uchar &a = buf[((size_t)y * size + x) * 4 + 3];
        a = (uchar)std::lround(a * circle_alpha(x, y, size));
      }
  auto *out = new Fl_RGB_Image(buf, size, size, 4);
  out->alloc_array = 1;
  return out;
}

bool avatar_png(Fl_Image *img, int size, std::string &png) {
  auto rgb = cover(img, size);
  if (!rgb) return false;
  std::unique_ptr<uchar[]> buf(new uchar[(size_t)size * size * 4]);
  crop_rgba(rgb.get(), size, buf.get());
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return false;
  }
  cairo_surface_flush(s);
  unsigned char *dst = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  for (int y = 0; y < size; y++) {
    uint32_t *row = (uint32_t *)(dst + (size_t)y * stride);
    for (int x = 0; x < size; x++) {
      const uchar *p = buf.get() + ((size_t)y * size + x) * 4;
      uint32_t a = p[3];
      // cairo wants premultiplied native-endian ARGB
      row[x] = a << 24 | (p[0] * a / 255) << 16 | (p[1] * a / 255) << 8 | (p[2] * a / 255);
    }
  }
  cairo_surface_mark_dirty(s);
  return surface_png(s, png);
}

bool initial_avatar_png(const std::string &user, const std::string &real_name, int size, std::string &png) {
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, size, size);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return false;
  }
  cairo_t *cr = cairo_create(s);
  unsigned long v = strtoul(avatar_color_hex(user).c_str() + 1, nullptr, 16);
  cairo_set_source_rgb(cr, ((v >> 16) & 255) / 255.0, ((v >> 8) & 255) / 255.0, (v & 255) / 255.0);
  cairo_arc(cr, size / 2.0, size / 2.0, size / 2.0, 0, 2 * M_PI);
  cairo_fill(cr);
  std::string letter = avatar_initial(user, real_name);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
  cairo_set_font_size(cr, size * 0.5);
  cairo_text_extents_t te;
  cairo_text_extents(cr, letter.c_str(), &te);
  cairo_move_to(cr, size / 2.0 - te.width / 2 - te.x_bearing, size / 2.0 - te.height / 2 - te.y_bearing);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_show_text(cr, letter.c_str());
  cairo_destroy(cr);
  return surface_png(s, png);
}

} // namespace fld
