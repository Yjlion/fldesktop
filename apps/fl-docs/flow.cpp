// fl-docs: flowing text laid out into pages (EPUB, OpenDocument text)
#include "flow.h"

#include <cairo-pdf.h>
#include <pango/pangocairo.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <map>

#include "images.h"

namespace docs::flow {

std::string escape(const std::string &text) {
  std::string o;
  o.reserve(text.size());
  for (char c : text) {
    switch (c) {
    case '&': o += "&amp;"; break;
    case '<': o += "&lt;"; break;
    case '>': o += "&gt;"; break;
    case '"': o += "&quot;"; break;
    case '\'': o += "&#39;"; break;
    default:
      // control characters are not allowed in markup (tabs and newlines are)
      if ((unsigned char)c < 0x20 && c != '\t' && c != '\n') o += ' ';
      else o += c;
    }
  }
  return o;
}

namespace {

struct Piece {
  enum Kind { Text, Image, Line, Box } kind = Text;
  double x = 0, y = 0, w = 0, h = 0; // on the page, points
  PangoLayout *layout = nullptr;     // Text
  double ly = 0;                     // Text: the layout's y at the piece's top
  bool searchable = true;            // not for bullets
  int image = -1;                    // Image
};

struct Line {
  double top, bottom; // points, in the layout
};

constexpr double kCellPad = 4;

} // namespace

struct Document::Impl {
  PangoContext *ctx = nullptr;
  PageStyle st;
  std::vector<std::vector<Piece>> pages;
  std::vector<PangoLayout *> layouts; // owned
  std::vector<std::string> images;    // bytes
  std::vector<std::pair<double, double>> image_px; // pixel sizes (0: unknown)
  std::map<int, cairo_surface_t *> decoded;
  std::list<int> order; // most recently used first
  double y = 0;         // where the next piece goes on the last page
  std::vector<Block> todo; // blocks still to lay out, from `next`
  size_t next = 0;

  Impl() {
    ctx = pango_font_map_create_context(pango_cairo_font_map_get_default());
    // points as user units, and the same line breaks at every zoom
    pango_cairo_context_set_resolution(ctx, 72);
    cairo_font_options_t *fo = cairo_font_options_create();
    cairo_font_options_set_hint_metrics(fo, CAIRO_HINT_METRICS_OFF);
    cairo_font_options_set_hint_style(fo, CAIRO_HINT_STYLE_NONE);
    cairo_font_options_set_antialias(fo, CAIRO_ANTIALIAS_GRAY);
    pango_cairo_context_set_font_options(ctx, fo);
    cairo_font_options_destroy(fo);
    pango_context_set_round_glyph_positions(ctx, FALSE);
  }
  ~Impl() {
    clear();
    g_object_unref(ctx);
  }
  void clear() {
    for (auto *l : layouts) g_object_unref(l);
    layouts.clear();
    for (auto &kv : decoded) cairo_surface_destroy(kv.second);
    decoded.clear();
    order.clear();
    images.clear();
    image_px.clear();
    pages.clear();
  }

  double content_w() const { return st.w - st.left - st.right; }
  double bottom() const { return st.h - st.bottom; }
  bool page_empty() const { return pages.back().empty(); }
  void new_page() {
    pages.emplace_back();
    y = st.top;
  }

  PangoLayout *make_layout(const Block &b, double width, const std::string &markup) {
    PangoLayout *l = pango_layout_new(ctx);
    layouts.push_back(l);
    pango_layout_set_width(l, (int)(std::max(10.0, width) * PANGO_SCALE));
    pango_layout_set_wrap(l, PANGO_WRAP_WORD_CHAR);
    PangoFontDescription *fd = pango_font_description_from_string(b.family.empty() ? st.family.c_str() : b.family.c_str());
    pango_font_description_set_size(fd, (int)((b.size > 0 ? b.size : st.size) * PANGO_SCALE));
    pango_layout_set_font_description(l, fd);
    pango_font_description_free(fd);
    switch (b.align) {
    case Block::Center: pango_layout_set_alignment(l, PANGO_ALIGN_CENTER); break;
    case Block::Right: pango_layout_set_alignment(l, PANGO_ALIGN_RIGHT); break;
    case Block::Justify: pango_layout_set_justify(l, TRUE); break;
    default: break;
    }
    if (b.first_indent != 0) pango_layout_set_indent(l, (int)(b.first_indent * PANGO_SCALE));
    pango_layout_set_line_spacing(l, 1.15f);
    // markup that does not parse is shown as text
    if (pango_parse_markup(markup.c_str(), -1, 0, nullptr, nullptr, nullptr, nullptr)) {
      pango_layout_set_markup(l, markup.c_str(), -1);
    } else {
      std::string stripped;
      bool tag = false;
      for (char c : markup) {
        if (c == '<') tag = true;
        else if (c == '>') tag = false;
        else if (!tag) stripped += c;
      }
      pango_layout_set_text(l, stripped.c_str(), -1);
    }
    return l;
  }

  static std::vector<Line> lines_of(PangoLayout *l) {
    std::vector<Line> v;
    PangoLayoutIter *it = pango_layout_get_iter(l);
    do {
      int y0, y1;
      pango_layout_iter_get_line_yrange(it, &y0, &y1);
      v.push_back({(double)y0 / PANGO_SCALE, (double)y1 / PANGO_SCALE});
    } while (pango_layout_iter_next_line(it));
    pango_layout_iter_free(it);
    return v;
  }

  void text(const Block &b) {
    if (b.markup.empty() && b.bullet.empty()) return;
    if (!page_empty() && y > st.top) y += b.space_before;
    double x = st.left + b.indent, width = content_w() - b.indent;
    PangoLayout *l = make_layout(b, width, b.markup.empty() ? " " : b.markup);
    std::vector<Line> lines = lines_of(l);
    size_t start = 0;
    bool first = true;
    while (start < lines.size()) {
      size_t k = start;
      while (k + 1 < lines.size() && lines[k + 1].bottom - lines[start].top <= bottom() - y) k++;
      if (lines[k].bottom - lines[start].top > bottom() - y && y > st.top) {
        new_page(); // not even one line fits
        continue;
      }
      Piece p;
      p.kind = Piece::Text;
      p.x = x;
      p.y = y;
      p.w = width;
      p.h = lines[k].bottom - lines[start].top;
      p.layout = l;
      p.ly = lines[start].top;
      pages.back().push_back(p);
      if (first && !b.bullet.empty()) {
        Block bb = b;
        bb.align = Block::Right;
        bb.first_indent = 0;
        PangoLayout *bl = make_layout(bb, 30, escape(b.bullet));
        Piece q;
        q.x = x - 36;
        q.y = y;
        q.w = 30;
        q.h = p.h;
        q.layout = bl;
        q.searchable = false;
        pages.back().push_back(q);
      }
      first = false;
      y += p.h;
      start = k + 1;
      if (start < lines.size()) new_page();
    }
    y += b.space_after;
  }

  void image(const Block &b) {
    int px_w = 0, px_h = 0;
    double w = b.image_w, h = b.image_h;
    if (!(w > 0 && h > 0)) {
      if (image_size(b.image, px_w, px_h)) {
        w = px_w * 0.75;
        h = px_h * 0.75;
      } else if (cairo_surface_t *s = decode_image(b.image, 1.0)) {
        w = cairo_image_surface_get_width(s) * 0.75;
        h = cairo_image_surface_get_height(s) * 0.75;
        cairo_surface_destroy(s);
      } else {
        return; // not an image we can show
      }
    }
    double maxh = st.h - st.top - st.bottom;
    double f = std::min({1.0, content_w() / w, maxh / h});
    w *= f;
    h *= f;
    if (y + h > bottom() && !page_empty()) new_page();
    if (!page_empty() && y > st.top) y += 4;
    Piece p;
    p.kind = Piece::Image;
    p.x = st.left + (content_w() - w) / 2;
    p.y = y;
    p.w = w;
    p.h = h;
    p.image = (int)images.size();
    images.push_back(b.image);
    pages.back().push_back(p);
    y += h + 6;
  }

  void rule() {
    if (y + 12 > bottom()) new_page();
    Piece p;
    p.kind = Piece::Line;
    p.x = st.left + content_w() * 0.25;
    p.y = y + 6;
    p.w = content_w() * 0.5;
    p.h = 0;
    pages.back().push_back(p);
    y += 12;
  }

  // a cell's paragraphs, one under the other; returns their height
  double cell(const std::vector<Block> &blocks, double x, double width, std::vector<Piece> &out) {
    double cy = 0;
    for (const Block &b : blocks) {
      if (b.kind != Block::Text || b.markup.empty()) continue;
      if (cy > 0) cy += b.space_before;
      PangoLayout *l = make_layout(b, width - b.indent, b.markup);
      int lw, lh;
      pango_layout_get_size(l, &lw, &lh);
      Piece p;
      p.kind = Piece::Text;
      p.x = x + b.indent;
      p.y = cy;
      p.w = width - b.indent;
      p.h = (double)lh / PANGO_SCALE;
      p.layout = l;
      out.push_back(p);
      cy += p.h + b.space_after * 0.5;
    }
    return cy;
  }

  void table(const Block &b) {
    size_t cols = 0;
    for (auto &r : b.rows) cols = std::max(cols, r.size());
    if (!cols) return;
    double cw = content_w() / (double)cols;
    if (!page_empty() && y > st.top) y += 6;
    for (auto &row : b.rows) {
      std::vector<Piece> pieces;
      double rh = 0;
      for (size_t c = 0; c < row.size(); c++) {
        std::vector<Piece> cp;
        double h = cell(row[c], st.left + c * cw + kCellPad, cw - 2 * kCellPad, cp);
        rh = std::max(rh, h);
        pieces.insert(pieces.end(), cp.begin(), cp.end());
      }
      rh += 2 * kCellPad;
      if (y + rh > bottom() && !page_empty() && y > st.top) new_page();
      for (size_t c = 0; c < cols; c++) {
        Piece box;
        box.kind = Piece::Box;
        box.x = st.left + c * cw;
        box.y = y;
        box.w = cw;
        box.h = rh;
        pages.back().push_back(box);
      }
      for (Piece p : pieces) {
        p.y += y + kCellPad;
        pages.back().push_back(p);
      }
      y += rh;
    }
    y += 6;
  }

  void add(const Block &b) {
    switch (b.kind) {
    case Block::Text: text(b); break;
    case Block::Image: image(b); break;
    case Block::Rule: rule(); break;
    case Block::PageBreak:
      if (!page_empty()) new_page();
      break;
    case Block::Table: table(b); break;
    }
  }

  cairo_surface_t *surface(int i) {
    auto it = decoded.find(i);
    if (it != decoded.end()) {
      order.remove(i);
      order.push_front(i);
      return it->second;
    }
    cairo_surface_t *s = decode_image(images[(size_t)i]);
    if (!s) return nullptr;
    decoded[i] = s;
    order.push_front(i);
    while (order.size() > 8) {
      cairo_surface_destroy(decoded[order.back()]);
      decoded.erase(order.back());
      order.pop_back();
    }
    return s;
  }

  void draw_page(cairo_t *cr, size_t page) {
    for (const Piece &p : pages[page]) {
      switch (p.kind) {
      case Piece::Text:
        cairo_save(cr);
        cairo_rectangle(cr, 0, p.y, st.w, p.h);
        cairo_clip(cr);
        cairo_set_source_rgb(cr, 0, 0, 0);
        cairo_move_to(cr, p.x, p.y - p.ly);
        pango_cairo_show_layout(cr, p.layout);
        cairo_restore(cr);
        break;
      case Piece::Image:
        if (cairo_surface_t *s = surface(p.image)) draw_image(cr, s, p.x, p.y, p.w, p.h);
        break;
      case Piece::Line:
      case Piece::Box:
        cairo_save(cr);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        cairo_set_line_width(cr, 0.75);
        if (p.kind == Piece::Line) {
          cairo_move_to(cr, p.x, p.y);
          cairo_line_to(cr, p.x + p.w, p.y);
        } else {
          cairo_rectangle(cr, p.x, p.y, p.w, p.h);
        }
        cairo_stroke(cr);
        cairo_restore(cr);
        break;
      }
    }
  }
};

Document::Document() : d(new Impl) {}
Document::~Document() = default;

void Document::layout(const std::vector<Block> &blocks, const PageStyle &style) {
  begin(blocks, style);
  while (!step(1e9)) {}
}

void Document::begin(std::vector<Block> blocks, const PageStyle &style) {
  d->clear();
  d->st = style;
  d->new_page();
  d->todo = std::move(blocks);
  d->next = 0;
}

bool Document::step(double seconds) {
  if (done()) return true;
  auto start = std::chrono::steady_clock::now();
  auto budget = std::chrono::duration<double>(seconds);
  // a block at a time (a long table or paragraph is laid out whole)
  while (d->next < d->todo.size()) {
    d->add(d->todo[d->next++]);
    if (std::chrono::steady_clock::now() - start >= budget) break;
  }
  if (d->next < d->todo.size()) return false;
  if (d->pages.size() > 1 && d->pages.back().empty()) d->pages.pop_back();
  std::vector<Block>().swap(d->todo);
  d->next = 0;
  return true;
}

bool Document::done() const { return d->todo.empty(); }

std::vector<PageSize> Document::sizes() const {
  return std::vector<PageSize>(d->pages.size(), PageSize{d->st.w, d->st.h});
}

bool Document::render(size_t page, double scale, Bitmap &out) {
  if (page >= d->pages.size()) return false;
  int w = (int)std::ceil(d->st.w * scale), h = (int)std::ceil(d->st.h * scale);
  if (w <= 0 || h <= 0 || (double)w * h > 120e6) return false;
  cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_RGB24, w, h);
  cairo_t *cr = cairo_create(s);
  cairo_set_source_rgb(cr, 1, 1, 1);
  cairo_paint(cr);
  cairo_scale(cr, scale, scale);
  d->draw_page(cr, page);
  cairo_destroy(cr);
  cairo_surface_flush(s);
  const unsigned char *src = cairo_image_surface_get_data(s);
  int stride = cairo_image_surface_get_stride(s);
  out.w = w;
  out.h = h;
  out.rgb.resize((size_t)w * h * 3);
  for (int yy = 0; yy < h; yy++) {
    const uint32_t *row = (const uint32_t *)(src + (size_t)yy * stride);
    unsigned char *o = out.rgb.data() + (size_t)yy * w * 3;
    for (int x = 0; x < w; x++) {
      o[3 * x] = (unsigned char)(row[x] >> 16);
      o[3 * x + 1] = (unsigned char)(row[x] >> 8);
      o[3 * x + 2] = (unsigned char)row[x];
    }
  }
  cairo_surface_destroy(s);
  return true;
}

std::vector<Hit> Document::find(const std::string &text) const {
  std::vector<Hit> hits;
  if (text.empty()) return hits;
  auto lower = [](std::string s) {
    for (char &c : s)
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
  };
  std::string q = lower(text);
  // the pieces of each layout, in page order
  std::vector<PangoLayout *> seen;
  std::map<PangoLayout *, std::vector<std::pair<size_t, const Piece *>>> parts;
  for (size_t pg = 0; pg < d->pages.size(); pg++)
    for (const Piece &p : d->pages[pg])
      if (p.kind == Piece::Text && p.searchable) {
        if (!parts.count(p.layout)) seen.push_back(p.layout);
        parts[p.layout].push_back({pg, &p});
      }
  for (PangoLayout *l : seen) {
    std::string t = lower(pango_layout_get_text(l));
    for (size_t at = t.find(q); at != std::string::npos; at = t.find(q, at + q.size())) {
      PangoRectangle a, b;
      pango_layout_index_to_pos(l, (int)at, &a);
      pango_layout_index_to_pos(l, (int)(at + q.size()), &b);
      double x0 = (double)a.x / PANGO_SCALE, y0 = (double)a.y / PANGO_SCALE, h = (double)a.height / PANGO_SCALE;
      double x1 = b.y == a.y ? (double)b.x / PANGO_SCALE : x0 + (double)a.width / PANGO_SCALE;
      if (x1 < x0) std::swap(x0, x1);
      for (auto &pp : parts[l]) {
        const Piece &p = *pp.second;
        if (y0 + 0.01 >= p.ly && y0 < p.ly + p.h - 0.01) {
          hits.push_back({(int)pp.first, p.x + x0, p.y + (y0 - p.ly), std::max(2.0, x1 - x0), h});
          break;
        }
      }
      if (hits.size() > 100000) return hits;
    }
  }
  std::stable_sort(hits.begin(), hits.end(), [](const Hit &a, const Hit &b) {
    return a.page != b.page ? a.page < b.page : a.y < b.y - 0.5 || (std::fabs(a.y - b.y) <= 0.5 && a.x < b.x);
  });
  return hits;
}

bool Document::write_pdf(const std::string &path, const std::string &title, std::string &err) {
  cairo_surface_t *s = cairo_pdf_surface_create(path.c_str(), d->st.w, d->st.h);
  if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
    err = cairo_status_to_string(cairo_surface_status(s));
    cairo_surface_destroy(s);
    return false;
  }
  if (!title.empty()) cairo_pdf_surface_set_metadata(s, CAIRO_PDF_METADATA_TITLE, title.c_str());
  cairo_pdf_surface_set_metadata(s, CAIRO_PDF_METADATA_CREATOR, "fl-docs");
  cairo_t *cr = cairo_create(s);
  for (size_t i = 0; i < d->pages.size(); i++) {
    d->draw_page(cr, i);
    cairo_show_page(cr);
  }
  cairo_status_t st = cairo_status(cr);
  cairo_destroy(cr);
  cairo_surface_finish(s);
  if (st == CAIRO_STATUS_SUCCESS) st = cairo_surface_status(s);
  cairo_surface_destroy(s);
  if (st != CAIRO_STATUS_SUCCESS) {
    err = cairo_status_to_string(st);
    return false;
  }
  return true;
}

} // namespace docs::flow
