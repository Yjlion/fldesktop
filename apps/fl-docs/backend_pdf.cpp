// fl-docs: PDF through poppler-cpp
#include <poppler-document.h>
#include <poppler-image.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>

#include <cstdint>

#include "backend.h"
#include "fld/util.h"

namespace docs {

namespace {

class PdfBackend : public Backend {
public:
  std::string path;
  std::unique_ptr<poppler::document> doc;
  std::vector<std::unique_ptr<poppler::page>> pages;

  bool render(size_t index, double scale, Bitmap &out) override {
    if (index >= pages.size() || !pages[index]) return false;
    poppler::page_renderer r;
    r.set_render_hint(poppler::page_renderer::antialiasing);
    r.set_render_hint(poppler::page_renderer::text_antialiasing);
    r.set_image_format(poppler::image::format_argb32);
    poppler::image img = r.render_page(pages[index].get(), 72.0 * scale, 72.0 * scale);
    if (!img.is_valid() || img.format() != poppler::image::format_argb32) return false;
    int w = img.width(), h = img.height(), bpr = img.bytes_per_row();
    out.w = w;
    out.h = h;
    out.rgb.resize((size_t)w * h * 3);
    const unsigned char *src = (const unsigned char *)img.const_data();
    for (int y = 0; y < h; y++) {
      const uint32_t *row = (const uint32_t *)(src + (size_t)y * bpr);
      unsigned char *o = out.rgb.data() + (size_t)y * w * 3;
      for (int x = 0; x < w; x++) {
        uint32_t p = row[x];
        uint32_t a = p >> 24;
        // premultiplied over white paper
        o[3 * x] = (unsigned char)(((p >> 16) & 0xff) + 255 - a);
        o[3 * x + 1] = (unsigned char)(((p >> 8) & 0xff) + 255 - a);
        o[3 * x + 2] = (unsigned char)((p & 0xff) + 255 - a);
      }
    }
    return true;
  }

  std::vector<Hit> find(const std::string &text) override {
    std::vector<Hit> hits;
    if (text.empty()) return hits;
    poppler::ustring u = poppler::ustring::from_utf8(text.c_str());
    for (size_t i = 0; i < pages.size(); i++) {
      poppler::page *p = pages[i].get();
      if (!p) continue;
      poppler::rectf r;
      auto dir = poppler::page::search_from_top;
      while (p->search(u, r, dir, poppler::case_insensitive)) {
        hits.push_back({(int)i, r.x(), r.y(), r.width(), r.height()});
        dir = poppler::page::search_next_result;
        if (hits.size() > 100000) return hits;
      }
    }
    return hits;
  }

  bool print_pdf(std::string &out, bool &temporary, std::string &) override {
    out = path;
    temporary = false;
    return true;
  }
};

} // namespace

std::unique_ptr<Backend> open_pdf(const std::string &path, std::string &err, const PasswordFn &password) {
  std::unique_ptr<poppler::document> d(poppler::document::load_from_file(path));
  if (!d) {
    err = "This is not a PDF file, or it is damaged.";
    return nullptr;
  }
  if (d->is_locked() && !password) {
    err = "The document is protected by a password.";
    return nullptr;
  }
  for (int tries = 0; tries < 3 && d->is_locked(); tries++) {
    std::string pw;
    if (!password(pw)) {
      err = "";
      return nullptr;
    }
    d->unlock(pw, pw);
  }
  if (d->is_locked()) {
    err = "Wrong password.";
    return nullptr;
  }
  auto b = std::make_unique<PdfBackend>();
  b->path = path;
  for (int i = 0; i < d->pages(); i++) {
    std::unique_ptr<poppler::page> p(d->create_page(i));
    PageSize s;
    if (p) {
      poppler::rectf r = p->page_rect();
      s.w = r.width();
      s.h = r.height();
      // pages turned by a quarter are shown turned
      auto o = p->orientation();
      if (o == poppler::page::landscape || o == poppler::page::seascape) std::swap(s.w, s.h);
    }
    b->sizes_.push_back(s);
    b->pages.push_back(std::move(p));
  }
  poppler::byte_array t = d->get_title().to_utf8();
  b->title_ = fld::trim(std::string(t.begin(), t.end()));
  b->doc = std::move(d);
  return b;
}

} // namespace docs
