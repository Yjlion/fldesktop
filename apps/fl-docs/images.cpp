// fl-docs: images in documents and comic books, decoded for cairo
#include "images.h"

#include <FL/Fl_BMP_Image.H>
#include <FL/Fl_GIF_Image.H>
#include <FL/Fl_JPEG_Image.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_SVG_Image.H>

#include <cstdint>
#include <memory>

namespace docs {

namespace {

bool starts(const std::string &d, const char *magic, size_t n) { return d.size() >= n && d.compare(0, n, magic, n) == 0; }

bool looks_svg(const std::string &d) {
  size_t n = std::min<size_t>(d.size(), 1024);
  return d.substr(0, n).find("<svg") != std::string::npos;
}

} // namespace

cairo_surface_t *decode_image(const std::string &data, double svg_scale) {
  if (data.empty() || data.size() > (256u << 20)) return nullptr;
  const unsigned char *u = (const unsigned char *)data.data();
  std::unique_ptr<Fl_RGB_Image> img;
  if (starts(data, "\x89PNG", 4)) img.reset(new Fl_PNG_Image(nullptr, u, (int)data.size()));
  else if (starts(data, "\xff\xd8", 2)) img.reset(new Fl_JPEG_Image(nullptr, u, (int)data.size()));
  else if (starts(data, "GIF8", 4)) {
    // a pixmap in FLTK: its first frame as RGBA
    Fl_GIF_Image gif(nullptr, u, data.size());
    if (!gif.fail() && gif.w() > 0 && gif.h() > 0) img.reset(new Fl_RGB_Image(&gif, FL_WHITE));
  }
  else if (starts(data, "BM", 2)) img.reset(new Fl_BMP_Image(nullptr, u, (long)data.size()));
  else if (looks_svg(data)) {
    auto *svg = new Fl_SVG_Image(nullptr, u, data.size());
    img.reset(svg);
    if (svg->w() > 0 && svg->h() > 0) {
      svg->resize((int)(svg->w() * svg_scale), (int)(svg->h() * svg_scale));
      svg->normalize();
    }
  }
  if (!img || img->fail() || img->w() <= 0 || img->h() <= 0 || !img->data() || !img->data()[0]) return nullptr;
  int w = img->data_w(), h = img->data_h(), d = img->d(), ld = img->ld() ? img->ld() : w * d;
  if (w > 16384 || h > 16384 || d < 1 || d > 4) return nullptr;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    cairo_surface_destroy(s);
    return nullptr;
  }
  cairo_surface_flush(s);
  unsigned char *dst = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  const unsigned char *src = (const unsigned char *)img->data()[0];
  for (int y = 0; y < h; y++) {
    const unsigned char *p = src + (size_t)y * ld;
    uint32_t *o = (uint32_t *)(dst + (size_t)y * stride);
    for (int x = 0; x < w; x++, p += d) {
      unsigned r, g, b, a = 255;
      if (d < 3) {
        r = g = b = p[0];
        if (d == 2) a = p[1];
      } else {
        r = p[0];
        g = p[1];
        b = p[2];
        if (d == 4) a = p[3];
      }
      // cairo wants premultiplied alpha
      r = r * a / 255;
      g = g * a / 255;
      b = b * a / 255;
      o[x] = (a << 24) | (r << 16) | (g << 8) | b;
    }
  }
  cairo_surface_mark_dirty(s);
  return s;
}

void draw_image(cairo_t *cr, cairo_surface_t *img, double x, double y, double w, double h) {
  int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
  if (iw <= 0 || ih <= 0 || w <= 0 || h <= 0) return;
  cairo_save(cr);
  cairo_rectangle(cr, x, y, w, h);
  cairo_clip(cr);
  cairo_translate(cr, x, y);
  cairo_scale(cr, w / iw, h / ih);
  cairo_set_source_surface(cr, img, 0, 0);
  cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
  cairo_paint(cr);
  cairo_restore(cr);
}

} // namespace docs
