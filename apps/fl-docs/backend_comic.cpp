// fl-docs: comic books (.cbz, also .cbr, .cb7, .cbt): one image per page
//
// The archive's images in natural name order ("page2" before "page10").
// Pages are read from the archive when they are drawn; only the page sizes
// are kept, from the image headers.
#include <cairo-pdf.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>

#include "archive_reader.h"
#include "backend.h"
#include "fld/util.h"
#include "images.h"

namespace docs {

namespace {

// one pixel is shown as one pixel at 100%
constexpr double kPointsPerPixel = 72.0 / 96.0;

bool hidden(const std::string &name) {
  if (name.compare(0, 9, "__MACOSX/") == 0) return true;
  std::string base = fld::basename(name);
  return base.empty() || base[0] == '.';
}

class ComicBackend : public Backend {
public:
  std::string path;
  std::vector<std::string> members;

  cairo_surface_t *page(size_t i) {
    std::string data, err;
    if (i >= members.size() || !read_member(path, members[i], data, err)) return nullptr;
    return decode_image(data);
  }

  bool render(size_t i, double scale, Bitmap &out) override {
    if (i >= sizes_.size()) return false;
    cairo_surface_t *img = page(i);
    int w = std::max(1, (int)std::lround(sizes_[i].w * scale)), h = std::max(1, (int)std::lround(sizes_[i].h * scale));
    if ((double)w * h > 120e6) {
      if (img) cairo_surface_destroy(img);
      return false;
    }
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, 1, 1, 1);
    cairo_paint(cr);
    if (img) draw_image(cr, img, 0, 0, w, h);
    cairo_destroy(cr);
    if (img) cairo_surface_destroy(img);
    cairo_surface_flush(s);
    const unsigned char *src = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    out.w = w;
    out.h = h;
    out.rgb.resize((size_t)w * h * 3);
    for (int y = 0; y < h; y++) {
      const uint32_t *row = (const uint32_t *)(src + (size_t)y * stride);
      unsigned char *o = out.rgb.data() + (size_t)y * w * 3;
      for (int x = 0; x < w; x++) {
        o[3 * x] = (unsigned char)(row[x] >> 16);
        o[3 * x + 1] = (unsigned char)(row[x] >> 8);
        o[3 * x + 2] = (unsigned char)row[x];
      }
    }
    cairo_surface_destroy(s);
    return true;
  }

  std::vector<Hit> find(const std::string &) override { return {}; }
  bool has_text() const override { return false; }

  bool print_pdf(std::string &out, bool &temporary, std::string &err) override {
    out = temp_pdf_path();
    temporary = true;
    cairo_surface_t *s = cairo_pdf_surface_create(out.c_str(), 612, 792);
    if (!title_.empty()) cairo_pdf_surface_set_metadata(s, CAIRO_PDF_METADATA_TITLE, title_.c_str());
    cairo_t *cr = cairo_create(s);
    for (size_t i = 0; i < sizes_.size(); i++) {
      cairo_pdf_surface_set_size(s, sizes_[i].w, sizes_[i].h);
      if (cairo_surface_t *img = page(i)) {
        draw_image(cr, img, 0, 0, sizes_[i].w, sizes_[i].h);
        cairo_surface_destroy(img);
      }
      cairo_show_page(cr);
    }
    cairo_status_t st = cairo_status(cr);
    cairo_destroy(cr);
    cairo_surface_finish(s);
    if (st == CAIRO_STATUS_SUCCESS) st = cairo_surface_status(s);
    cairo_surface_destroy(s);
    if (st != CAIRO_STATUS_SUCCESS) err = cairo_status_to_string(st);
    return st == CAIRO_STATUS_SUCCESS;
  }
};

} // namespace

std::unique_ptr<Backend> open_comic(const std::string &path, std::string &err) {
  std::vector<std::string> names;
  if (!list_members(path, names, err)) return nullptr;
  auto b = std::make_unique<ComicBackend>();
  b->path = path;
  for (auto &n : names)
    if (is_image_name(n) && !hidden(n)) b->members.push_back(n);
  std::sort(b->members.begin(), b->members.end(), natural_less);
  if (b->members.empty()) {
    err = "There are no pictures in this comic book.";
    return nullptr;
  }
  // the sizes from the image headers, read in one pass
  std::map<std::string, std::string> heads;
  std::string e2;
  std::vector<std::string> want = b->members;
  std::sort(want.begin(), want.end());
  read_members(path, [&](const std::string &n) { return std::binary_search(want.begin(), want.end(), n); }, heads, e2,
               1u << 30, 1u << 30, 256 * 1024);
  for (auto &m : b->members) {
    int w = 0, h = 0;
    auto it = heads.find(m);
    if (it != heads.end() && !image_size(it->second, w, h)) {
      if (cairo_surface_t *s = decode_image(it->second)) {
        w = cairo_image_surface_get_width(s);
        h = cairo_image_surface_get_height(s);
        cairo_surface_destroy(s);
      }
    }
    if (w <= 0 || h <= 0) {
      w = 800;
      h = 1200;
    }
    b->sizes_.push_back({w * kPointsPerPixel, h * kPointsPerPixel});
  }
  b->title_ = fld::basename(path);
  size_t dot = b->title_.rfind('.');
  if (dot != std::string::npos && dot > 0) b->title_ = b->title_.substr(0, dot);
  return b;
}

} // namespace docs
