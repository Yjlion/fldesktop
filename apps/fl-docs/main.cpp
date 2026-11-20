// fl-docs: a document viewer: PDF (poppler-cpp), EPUB, OpenDocument text and
// comic books (backend.h)
//
// Pages scroll continuously in one strip, with page thumbnails on the left.
// Zoom fits the width or the page or is set in steps; search finds text on
// all pages and highlights it; printing goes to CUPS with copies, a page
// range and two-sided options.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "backend.h"
#include "docs_util.h"
#include "fld/file_dialog.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#ifdef FLD_HAVE_PRINTERS
#include "cupsclient.h"
#endif

namespace {

using docs::Fit;

// ---- the document ----------------------------------------------------------

using docs::Hit;

struct Doc {
  std::unique_ptr<docs::Backend> backend;
  std::vector<docs::PageSize> sizes;
  std::string path, title;
};

Doc g_doc;

bool load_document(const std::string &path, std::string &err, bool interactive = true) {
  docs::PasswordFn ask;
  if (interactive)
    ask = [&path](std::string &pw) {
      const char *p = fl_password("%s is protected. Password:", "", fld::basename(path).c_str());
      if (!p) return false;
      pw = p;
      return true;
    };
  std::unique_ptr<docs::Backend> b = docs::open_document(path, err, ask);
  if (!b) return false;
  Doc nd;
  nd.path = path;
  nd.sizes = b->sizes();
  nd.title = b->title();
  nd.backend = std::move(b);
  g_doc = std::move(nd);
  return true;
}

// All the places a text appears, page by page.
std::vector<Hit> find_all(const std::string &text) {
  if (text.empty() || !g_doc.backend) return {};
  return g_doc.backend->find(text);
}

// A page as an FLTK image at a scale (pixels per point).
Fl_RGB_Image *render(int index, double scale) {
  if (!g_doc.backend || index < 0 || index >= (int)g_doc.sizes.size()) return nullptr;
  docs::Bitmap bm;
  if (!g_doc.backend->render((size_t)index, scale, bm) || bm.w <= 0 || bm.h <= 0) return nullptr;
  uchar *rgb = new uchar[bm.rgb.size()];
  memcpy(rgb, bm.rgb.data(), bm.rgb.size());
  Fl_RGB_Image *im = new Fl_RGB_Image(rgb, bm.w, bm.h, 3);
  im->alloc_array = 1;
  return im;
}

// ---- a strip of pages (the document view and the thumbnails) ----------------

class PageStrip : public Fl_Group {
public:
  docs::Layout lay;
  double zoom = 1;
  Fit fit = Fit::Width;
  bool thumbs;
  int top = 0, left = 0;
  int selected = -1;                // thumbnails: the current page
  const std::vector<Hit> *hits = nullptr;
  int current_hit = -1;
  std::function<void(int)> on_page; // thumbnails: clicked; view: the current page changed
  std::function<void(int)> on_zoom; // Ctrl+wheel: +1 / -1
  Fl_Scrollbar *vs, *hs;

  PageStrip(int X, int Y, int W, int H, bool thumbnails) : Fl_Group(X, Y, W, H), thumbs(thumbnails) {
    vs = new Fl_Scrollbar(X + W - SB, Y, SB, H);
    vs->callback([](Fl_Widget *w, void *) { ((PageStrip *)w->parent())->scroll_to(((Fl_Scrollbar *)w)->value(), -1); });
    hs = new Fl_Scrollbar(X, Y + H - SB, W - SB, SB);
    hs->type(FL_HORIZONTAL);
    hs->callback([](Fl_Widget *w, void *) { ((PageStrip *)w->parent())->scroll_to(-1, ((Fl_Scrollbar *)w)->value()); });
    end();
    box(FL_FLAT_BOX);
    color(fld::palette().bg2);
  }

  static constexpr int SB = 14;
  int view_w() const { return w() - (vs->visible() ? SB : 0); }
  int view_h() const { return h() - (hs->visible() ? SB : 0); }

  void relayout(bool keep_page = true) {
    int page = keep_page && !lay.y.empty() ? docs::current_page(lay, top, view_h()) : 0;
    double frac = 0;
    if (page >= 0 && page < (int)lay.y.size() && lay.h[page] > 0) frac = (double)(top - lay.y[page]) / lay.h[page];
    if (thumbs) {
      // pages as wide as the strip
      double maxw = 1;
      for (auto &s : g_doc.sizes) maxw = std::max(maxw, s.w);
      zoom = (w() - SB - 2 * docs::kGap) / (maxw * docs::kPixelsPerPoint);
    } else if (fit != Fit::None) {
      zoom = docs::fit_zoom(g_doc.sizes, std::max(0, page), fit, w() - SB, h());
    }
    lay = docs::layout(g_doc.sizes, zoom, w() - SB);
    cache.clear();
    update_scrollbars();
    if (keep_page && page >= 0 && page < (int)lay.y.size()) scroll_to(lay.y[page] + (int)(frac * lay.h[page]), left);
    else scroll_to(0, 0);
  }

  void update_scrollbars() {
    bool need_h = !thumbs && lay.width > w() - SB;
    if (need_h) hs->show();
    else hs->hide();
    vs->resize(x() + w() - SB, y(), SB, h() - (need_h ? SB : 0));
    hs->resize(x(), y() + h() - SB, w() - SB, SB);
    vs->value(top, view_h(), 0, std::max(lay.height, view_h()));
    vs->linesize(48);
    hs->value(left, view_w(), 0, std::max(lay.width, view_w()));
    hs->linesize(48);
  }

  void scroll_to(int t, int l) {
    if (t >= 0) top = std::max(0, std::min(t, lay.height - view_h()));
    if (l >= 0) left = std::max(0, std::min(l, lay.width - view_w()));
    update_scrollbars();
    redraw();
    if (!thumbs && on_page) on_page(current());
  }

  int current() const { return docs::current_page(lay, top, view_h()); }

  void show_page(int p) {
    if (p < 0 || p >= (int)lay.y.size()) return;
    scroll_to(lay.y[p] - docs::kGap, -1);
  }

  // thumbnails: keep the selected page in sight
  void reveal(int p) {
    if (p < 0 || p >= (int)lay.y.size()) return;
    if (lay.y[p] < top || lay.y[p] + lay.h[p] > top + view_h()) scroll_to(lay.y[p] - view_h() / 3, -1);
    else redraw();
  }

  void reveal_rect(const docs::Rect &r) {
    int t = top, l = left;
    if (r.y < top + 40 || r.y + r.h > top + view_h() - 40) t = r.y - view_h() / 3;
    if (r.x < left || r.x + r.w > left + view_w()) l = r.x - view_w() / 3;
    scroll_to(t, l);
  }

  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    if (!g_doc.sizes.empty()) relayout(true);
    else update_scrollbars();
  }

  int handle(int e) override {
    if (e == FL_MOUSEWHEEL && Fl::event_inside(this)) {
      if (!thumbs && (Fl::event_state() & FL_CTRL)) {
        if (on_zoom && Fl::event_dy()) on_zoom(Fl::event_dy() < 0 ? 1 : -1);
        return 1;
      }
      scroll_to(top + Fl::event_dy() * 60, left + Fl::event_dx() * 60);
      return 1;
    }
    if (e == FL_PUSH && thumbs && Fl::event_inside(x(), y(), view_w(), view_h())) {
      int p = docs::page_at(lay, Fl::event_y() - y() + top);
      if (p >= 0 && on_page) on_page(p);
      return 1;
    }
    if (e == FL_PUSH && !thumbs && Fl::event_inside(x(), y(), view_w(), view_h()) && Fl::event_button() == FL_LEFT_MOUSE) {
      drag_x = Fl::event_x();
      drag_y = Fl::event_y();
      fl_cursor(FL_CURSOR_MOVE);
      return 1;
    }
    if (e == FL_DRAG && !thumbs) {
      scroll_to(top - (Fl::event_y() - drag_y), left - (Fl::event_x() - drag_x));
      drag_x = Fl::event_x();
      drag_y = Fl::event_y();
      return 1;
    }
    if (e == FL_RELEASE && !thumbs) {
      fl_cursor(FL_CURSOR_DEFAULT);
      return 1;
    }
    return Fl_Group::handle(e);
  }

  void draw() override {
    fl_push_clip(x(), y(), view_w(), view_h());
    fl_color(color());
    fl_rectf(x(), y(), view_w(), view_h());
    int ox = x() - left, oy = y() - top;
    for (size_t i = 0; i < lay.y.size(); i++) {
      int py = lay.y[i] + oy;
      if (py > y() + view_h()) break;
      if (py + lay.h[i] < y()) continue;
      int px = lay.x[i] + ox;
      // a soft frame
      fl_color(fl_darker(color()));
      fl_rect(px - 1, py - 1, lay.w[i] + 2, lay.h[i] + 2);
      if (thumbs && (int)i == selected) {
        fl_color(fld::palette().accent);
        fl_line_style(FL_SOLID, 3);
        fl_rect(px - 4, py - 4, lay.w[i] + 8, lay.h[i] + 8);
        fl_line_style(0);
      }
      Fl_RGB_Image *img = image(i);
      if (img) img->draw(px, py, lay.w[i], lay.h[i]);
      else {
        fl_color(FL_WHITE);
        fl_rectf(px, py, lay.w[i], lay.h[i]);
      }
      if (thumbs) {
        char n[16];
        snprintf(n, sizeof n, "%d", (int)i + 1);
        fl_font(FL_HELVETICA, 11);
        fl_color(fld::palette().fg_dim);
        fl_draw(n, px, py + lay.h[i], lay.w[i], docs::kGap, FL_ALIGN_CENTER);
      }
    }
    // search hits
    if (!thumbs && hits) {
      for (size_t k = 0; k < hits->size(); k++) {
        const Hit &h = (*hits)[k];
        docs::Rect r = docs::to_strip(lay, h.page, h.x, h.y, h.w, h.h);
        if (r.y + oy > y() + view_h() || r.y + r.h + oy < y()) continue;
        bool cur = (int)k == current_hit;
        fl_color(cur ? fl_rgb_color(255, 140, 0) : fl_rgb_color(240, 200, 0));
        fl_line_style(FL_SOLID, cur ? 3 : 2);
        fl_rect(r.x + ox - 2, r.y + oy - 2, r.w + 4, r.h + 4);
        fl_line_style(0);
      }
    }
    fl_pop_clip();
    draw_child(*vs);
    if (hs->visible()) draw_child(*hs);
    if (hs->visible() && vs->visible()) {
      fl_color(FL_BACKGROUND_COLOR);
      fl_rectf(x() + w() - SB, y() + h() - SB, SB, SB);
    }
  }

  void clear_cache() { cache.clear(); }

private:
  int drag_x = 0, drag_y = 0;
  std::map<size_t, std::unique_ptr<Fl_RGB_Image>> cache;
  std::list<size_t> order; // most recently used first

  Fl_RGB_Image *image(size_t i) {
    auto it = cache.find(i);
    if (it != cache.end()) {
      order.remove(i);
      order.push_front(i);
      return it->second.get();
    }
    Fl_RGB_Image *im = render((int)i, lay.scale());
    if (!im) return nullptr;
    cache[i].reset(im);
    order.push_front(i);
    size_t limit = thumbs ? 64 : 12;
    while (order.size() > limit) {
      cache.erase(order.back());
      order.pop_back();
    }
    return im;
  }
};

// ---- the window ------------------------------------------------------------

Fl_Double_Window *g_win;
fld::Toolbar *g_bar;
PageStrip *g_view, *g_thumbs;
Fl_Int_Input *g_page_in;
Fl_Box *g_page_of, *g_found;
Fl_Choice *g_zoom;
Fl_Input *g_search;
std::vector<Hit> g_hits;
std::string g_query, g_last_dir;
std::string g_title_buf;

struct ZoomItem {
  const char *label;
  Fit fit;
  double zoom;
};
const ZoomItem kZooms[] = {{"Fit width", Fit::Width, 0}, {"Fit page", Fit::Page, 0}, {"50%", Fit::None, 0.5},
                           {"75%", Fit::None, 0.75},     {"100%", Fit::None, 1.0},   {"125%", Fit::None, 1.25},
                           {"150%", Fit::None, 1.5},     {"200%", Fit::None, 2.0},   {"400%", Fit::None, 4.0}};

void update_title() {
  std::string name = g_doc.path.empty() ? "" : (g_doc.title.empty() ? fld::basename(g_doc.path) : g_doc.title);
  g_title_buf = name.empty() ? "Documents" : name + " – Documents";
  g_win->label(g_title_buf.c_str());
}

void update_zoom_label() {
  // the zoom in effect, shown in the choice's own text when it is not a fit
  static char buf[32];
  for (size_t i = 0; i < sizeof kZooms / sizeof kZooms[0]; i++) {
    const ZoomItem &z = kZooms[i];
    if ((g_view->fit != Fit::None && z.fit == g_view->fit) ||
        (g_view->fit == Fit::None && z.fit == Fit::None && std::fabs(z.zoom - g_view->zoom) < 1e-3)) {
      g_zoom->value((int)i);
      return;
    }
  }
  snprintf(buf, sizeof buf, "%d%%", (int)std::lround(g_view->zoom * 100));
  g_zoom->value(-1);
  g_zoom->copy_label("");
  g_zoom->tooltip(buf);
  g_zoom->redraw();
}

void page_changed(int p) {
  char b[16];
  snprintf(b, sizeof b, "%d", p + 1);
  if (Fl::focus() != g_page_in) g_page_in->value(b);
  if (g_thumbs->selected != p) {
    g_thumbs->selected = p;
    g_thumbs->reveal(p);
  }
}

void set_zoom(Fit fit, double zoom) {
  g_view->fit = fit;
  if (fit == Fit::None) g_view->zoom = std::max(0.1, std::min(8.0, zoom));
  g_view->relayout(true);
  update_zoom_label();
}

void zoom_by(int dir) { set_zoom(Fit::None, docs::zoom_step(g_view->zoom, dir)); }

void show_hit(int k) {
  if (g_hits.empty()) return;
  k = ((k % (int)g_hits.size()) + (int)g_hits.size()) % (int)g_hits.size();
  g_view->current_hit = k;
  const Hit &h = g_hits[k];
  g_view->reveal_rect(docs::to_strip(g_view->lay, h.page, h.x, h.y, h.w, h.h));
  static char buf[64];
  snprintf(buf, sizeof buf, "%d of %d", k + 1, (int)g_hits.size());
  g_found->copy_label(buf);
}

void search(int dir) {
  std::string q = fld::trim(g_search->value());
  if (q != g_query) {
    g_query = q;
    fl_cursor(FL_CURSOR_WAIT);
    Fl::flush();
    g_hits = find_all(q);
    fl_cursor(FL_CURSOR_DEFAULT);
    g_view->hits = &g_hits;
    g_view->current_hit = -1;
    if (g_hits.empty()) {
      bool text = g_doc.backend && g_doc.backend->has_text();
      g_found->copy_label(q.empty() ? "" : text ? "Not found" : "No text");
      g_view->redraw();
      return;
    }
    // the first hit from the current page on
    int cur = g_view->current(), k = 0;
    while (k < (int)g_hits.size() && g_hits[k].page < cur) k++;
    show_hit(k == (int)g_hits.size() ? 0 : k);
    return;
  }
  if (!g_hits.empty()) show_hit(g_view->current_hit + dir);
}

bool open_file(const std::string &path) {
  std::string err;
  fl_cursor(FL_CURSOR_WAIT);
  bool ok = load_document(path, err);
  fl_cursor(FL_CURSOR_DEFAULT);
  if (!ok) {
    if (!err.empty()) fl_alert("Cannot open %s.\n%s", fld::basename(path).c_str(), err.c_str());
    return false;
  }
  g_last_dir = fld::dirname(path);
  g_hits.clear();
  g_query.clear();
  g_found->copy_label("");
  g_view->hits = nullptr;
  g_view->top = g_view->left = 0;
  g_view->relayout(false);
  g_thumbs->top = 0;
  g_thumbs->relayout(false);
  static char of[32];
  snprintf(of, sizeof of, "of %d", (int)g_doc.sizes.size());
  g_page_of->label(of);
  page_changed(0);
  update_title();
  update_zoom_label();
  return true;
}

void open_dialog() {
  fld::FileDialogOptions o;
  o.title = "Open Document";
  o.folder = g_last_dir.empty() ? fld::user_dir("DOCUMENTS") : g_last_dir;
  if (o.folder.empty() || !fld::is_dir(o.folder)) o.folder = fld::home_dir();
  o.filters = {{"Documents", {"*.pdf", "*.epub", "*.odt", "*.ott", "*.cbz", "*.cbr", "*.cb7", "*.cbt"},
                {"application/pdf", "application/epub+zip", "application/vnd.oasis.opendocument.text",
                 "application/vnd.comicbook+zip", "application/x-cbz", "application/vnd.comicbook-rar"}},
               {"PDF documents", {"*.pdf"}, {"application/pdf"}},
               {"E-books", {"*.epub"}, {"application/epub+zip"}},
               {"OpenDocument text", {"*.odt", "*.ott"}, {"application/vnd.oasis.opendocument.text"}},
               {"Comic books", {"*.cbz", "*.cbr", "*.cb7", "*.cbt"}, {"application/vnd.comicbook+zip"}},
               {"All files", {"*"}, {}}};
  fld::FileDialogResult r = fld::run_file_dialog(o);
  if (r.ok && !r.paths.empty()) open_file(r.paths[0]);
}

#ifdef FLD_HAVE_PRINTERS
void print_dialog() {
  if (g_doc.path.empty() || !g_doc.backend) return;
  std::vector<prn::Printer> ps = prn::printers();
  if (ps.empty()) {
    fl_alert("No printer is set up. Add one in Printers.");
    return;
  }
  Fl_Double_Window d(420, 250, "Print");
  d.set_modal();
  int y = 16;
  Fl_Choice printer(130, y, 270, 28, "Printer:");
  int def = 0;
  for (size_t i = 0; i < ps.size(); i++) {
    printer.add(fld::escape_at(ps[i].info.empty() ? ps[i].name : ps[i].info).c_str());
    if (ps[i].is_default) def = (int)i;
  }
  printer.value(def);
  y += 38;
  Fl_Spinner copies(130, y, 70, 28, "Copies:");
  copies.range(1, 99);
  copies.value(1);
  y += 38;
  Fl_Input pages(130, y, 270, 28, "Pages:");
  pages.value("All");
  pages.tooltip("For example 1-3, 5, 8-");
  y += 38;
  Fl_Choice sides(130, y, 270, 28, "Sides:");
  sides.add("One side|Both sides (long edge)|Both sides (short edge)");
  sides.value(0);
  Fl_Button cancel(170, 204, 110, 30, "Cancel");
  Fl_Return_Button ok(290, 204, 110, 30, "Print");
  int result = 0;
  cancel.callback([](Fl_Widget *w, void *r) { *(int *)r = 2; }, &result);
  ok.callback([](Fl_Widget *w, void *r) { *(int *)r = 1; }, &result);
  d.callback([](Fl_Widget *w, void *r) { *(int *)r = 2; }, &result);
  d.end();
  d.show();
  for (;;) {
    while (!result && d.shown()) Fl::wait();
    if (result != 1) return;
    bool valid;
    std::string range = docs::page_ranges(pages.value(), (int)g_doc.sizes.size(), &valid);
    if (!valid) {
      fl_alert("The pages are not valid. Use numbers and ranges such as 1-3, 5, 8-.");
      result = 0;
      continue;
    }
    std::vector<std::pair<std::string, std::string>> opts;
    if (copies.value() > 1) opts.push_back({"copies", std::to_string((int)copies.value())});
    if (!range.empty()) opts.push_back({"page-ranges", range});
    const char *sd[] = {"one-sided", "two-sided-long-edge", "two-sided-short-edge"};
    opts.push_back({"sides", sd[std::max(0, sides.value())]});
    prn::Error err;
    std::string title = g_doc.title.empty() ? fld::basename(g_doc.path) : g_doc.title;
    // EPUB, ODT and comic books go to the printer as a PDF made for it
    std::string pdf, perr;
    bool temporary = false;
    fl_cursor(FL_CURSOR_WAIT);
    Fl::flush();
    bool made = g_doc.backend->print_pdf(pdf, temporary, perr);
    fl_cursor(FL_CURSOR_DEFAULT);
    if (!made) {
      fl_alert("Printing failed: %s", perr.c_str());
      if (temporary) unlink(pdf.c_str());
      return;
    }
    if (!prn::print_file(ps[printer.value()].name, pdf, title, opts, &err))
      fl_alert("Printing failed: %s", err.c_str());
    if (temporary) unlink(pdf.c_str());
    return;
  }
}
#endif

void goto_page(int p) {
  if (g_doc.sizes.empty()) return;
  p = std::max(0, std::min((int)g_doc.sizes.size() - 1, p));
  g_view->show_page(p);
}

enum Cmd { OPEN = 1, PRINT, PREV, NEXT, ZOOM_IN, ZOOM_OUT, THUMBS, FIND_PREV, FIND_NEXT, ABOUT };

void relayout_window() {
  int TB = g_bar->h(), W = g_win->w(), H = g_win->h();
  int tw = g_thumbs->visible() ? 180 : 0;
  g_thumbs->resize(0, TB, tw, H - TB);
  g_view->resize(tw, TB, W - tw, H - TB);
  g_win->redraw();
}

void command(Cmd c) {
  switch (c) {
  case OPEN: open_dialog(); break;
#ifdef FLD_HAVE_PRINTERS
  case PRINT: print_dialog(); break;
#else
  case PRINT: break;
#endif
  case PREV: goto_page(g_view->current() - 1); break;
  case NEXT: goto_page(g_view->current() + 1); break;
  case ZOOM_IN: zoom_by(1); break;
  case ZOOM_OUT: zoom_by(-1); break;
  case THUMBS:
    if (g_thumbs->visible()) g_thumbs->hide();
    else g_thumbs->show();
    relayout_window();
    break;
  case FIND_PREV: search(-1); break;
  case FIND_NEXT: search(1); break;
  case ABOUT: fld::about_dialog("Documents", "fl-docs", "A document viewer for fldesktop: PDF, EPUB, OpenDocument text and comic books."); break;
  }
}

void cmd_cb(Fl_Widget *, void *d) { command((Cmd)(long)d); }

class DocWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  void resize(int X, int Y, int W, int H) override {
    Fl_Window::resize(X, Y, W, H);
    g_bar->resize(0, 0, W, g_bar->h());
    relayout_window();
  }
  int handle(int e) override {
    // files dropped onto the window
    if (e == FL_DND_ENTER || e == FL_DND_DRAG || e == FL_DND_RELEASE) return 1;
    if (e == FL_PASTE) {
      std::string t = fld::trim(Fl::event_text());
      std::string first = t.substr(0, t.find('\n'));
      if (fld::starts_with(first, "file://")) first = fld::uri_decode(first.substr(7));
      if (!first.empty()) open_file(fld::trim(first));
      return 1;
    }
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key();
      bool ctrl = Fl::event_state() & FL_CTRL;
      bool typing = Fl::focus() == g_search || Fl::focus() == g_page_in;
      if (ctrl && k == 'o') return command(OPEN), 1;
      if (ctrl && k == 'p') return command(PRINT), 1;
      if (ctrl && k == 'f') {
        g_search->take_focus();
        g_search->insert_position(0, g_search->size());
        return 1;
      }
      if (k == FL_F + 3) return command((Fl::event_state() & FL_SHIFT) ? FIND_PREV : FIND_NEXT), 1;
      if (k == FL_F + 9) return command(THUMBS), 1;
      if (ctrl && (k == '=' || k == '+' || k == FL_KP + '+')) return command(ZOOM_IN), 1;
      if (ctrl && (k == '-' || k == FL_KP + '-')) return command(ZOOM_OUT), 1;
      if (ctrl && k == '0') return set_zoom(Fit::None, 1.0), 1;
      if (k == FL_Escape && Fl::focus() == g_search) {
        g_view->take_focus();
        return 1;
      }
      if (typing) return Fl_Double_Window::handle(e);
      int vh = g_view->view_h();
      switch (k) {
      case FL_Page_Down: case ' ': g_view->scroll_to(g_view->top + vh - 40, -1); return 1;
      case FL_Page_Up: case FL_BackSpace: g_view->scroll_to(g_view->top - vh + 40, -1); return 1;
      case FL_Down: g_view->scroll_to(g_view->top + 60, -1); return 1;
      case FL_Up: g_view->scroll_to(g_view->top - 60, -1); return 1;
      case FL_Right: g_view->scroll_to(-1, g_view->left + 60); return 1;
      case FL_Left: g_view->scroll_to(-1, g_view->left - 60); return 1;
      case FL_Home: goto_page(0); return 1;
      case FL_End: goto_page((int)g_doc.sizes.size() - 1); return 1;
      case 'n': return command(NEXT), 1;
      case 'p': return command(PREV), 1;
      }
    }
    return Fl_Double_Window::handle(e);
  }
};

// --info: page count, title and (with --find) the hits, for scripts and tests
int info(const std::string &path, const std::string &find) {
  std::string err;
  if (!load_document(path, err, false)) {
    fprintf(stderr, "fl-docs: %s: %s\n", path.c_str(), err.c_str());
    return 1;
  }
  printf("pages %d\n", (int)g_doc.sizes.size());
  printf("title %s\n", g_doc.title.c_str());
  for (size_t i = 0; i < g_doc.sizes.size(); i++)
    printf("page %d %.0fx%.0f\n", (int)i + 1, g_doc.sizes[i].w, g_doc.sizes[i].h);
  if (!find.empty())
    for (const Hit &h : find_all(find)) printf("hit %d %.0f %.0f %.0f %.0f\n", h.page + 1, h.x, h.y, h.w, h.h);
  return 0;
}

// --to-pdf: the PDF that printing sends (the file itself for a PDF)
int to_pdf(const std::string &path, const std::string &out) {
  std::string err;
  if (!load_document(path, err, false)) {
    fprintf(stderr, "fl-docs: %s: %s\n", path.c_str(), err.c_str());
    return 1;
  }
  std::string pdf;
  bool temporary = false;
  if (!g_doc.backend->print_pdf(pdf, temporary, err)) {
    fprintf(stderr, "fl-docs: %s\n", err.c_str());
    if (temporary) unlink(pdf.c_str());
    return 1;
  }
  std::string data;
  bool ok = fld::read_file(pdf, data, (size_t)2 << 30) && fld::write_file(out, data);
  if (temporary) unlink(pdf.c_str());
  if (!ok) fprintf(stderr, "fl-docs: cannot write %s\n", out.c_str());
  return ok ? 0 : 1;
}

} // namespace

int main(int argc, char **argv) {
  std::string file, find, pdf_out;
  bool want_info = false;
  int start_page = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-docs [--page N] [FILE]\n"
             "       fl-docs --info FILE [--find TEXT]\n"
             "       fl-docs --to-pdf FILE OUT.pdf\n"
             "FILE: PDF, EPUB, OpenDocument text (.odt) or comic book (.cbz, .cbr, .cb7)\n");
      return 0;
    }
    if (a == "--info") want_info = true;
    else if (a == "--to-pdf" && i + 2 < argc) {
      file = argv[++i];
      pdf_out = argv[++i];
    }
    else if (a == "--find" && i + 1 < argc) find = argv[++i];
    else if (a == "--page" && i + 1 < argc) start_page = atoi(argv[++i]) - 1;
    else if (a[0] != '-' && file.empty()) file = fld::starts_with(a, "file://") ? fld::uri_decode(a.substr(7)) : a;
  }
  if (want_info) return file.empty() ? 2 : info(file, find);
  if (!pdf_out.empty()) return to_pdf(file, pdf_out);

  fld::init_app("fl-docs", argc, argv);
  const int TB = 40, W = 1000, H = 760;
  g_win = new DocWindow(W, H, "Documents");
  g_bar = new fld::Toolbar(0, 0, W, TB);
  g_bar->color(fld::palette().bg);
  g_bar->add_button("document-open", "Open (Ctrl+O)", cmd_cb, (void *)OPEN);
#ifdef FLD_HAVE_PRINTERS
  g_bar->add_button("document-print", "Print (Ctrl+P)", cmd_cb, (void *)PRINT);
#endif
  g_bar->add_separator();
  g_bar->add_button("view-grid", "Page thumbnails (F9)", cmd_cb, (void *)THUMBS);
  g_bar->add_button("go-up", "Previous page (P)", cmd_cb, (void *)PREV);
  g_bar->add_button("go-down", "Next page (N)", cmd_cb, (void *)NEXT);
  g_page_in = new Fl_Int_Input(0, 6, 50, 28);
  g_page_in->when(FL_WHEN_ENTER_KEY);
  g_page_in->callback([](Fl_Widget *w, void *) {
    goto_page(atoi(((Fl_Int_Input *)w)->value()) - 1);
    g_view->take_focus();
  });
  g_bar->add_widget(g_page_in);
  g_page_of = new Fl_Box(0, 6, 64, 28, "");
  g_page_of->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_bar->add_widget(g_page_of);
  g_bar->add_separator();
  g_bar->add_button("zoom-out", "Zoom out (Ctrl+−)", cmd_cb, (void *)ZOOM_OUT);
  g_zoom = new Fl_Choice(0, 6, 110, 28);
  for (const ZoomItem &z : kZooms) g_zoom->add(z.label);
  g_zoom->callback([](Fl_Widget *w, void *) {
    int i = ((Fl_Choice *)w)->value();
    if (i >= 0) set_zoom(kZooms[i].fit, kZooms[i].zoom);
  });
  g_bar->add_widget(g_zoom);
  g_bar->add_button("zoom-in", "Zoom in (Ctrl++)", cmd_cb, (void *)ZOOM_IN);
  g_bar->add_separator();
  g_search = new Fl_Input(0, 6, 200, 28);
  g_search->when(FL_WHEN_ENTER_KEY_ALWAYS);
  g_search->tooltip("Find in the document (Ctrl+F, Enter, F3)");
  g_search->callback([](Fl_Widget *, void *) { search((Fl::event_state() & FL_SHIFT) ? -1 : 1); });
  g_bar->add_widget(g_search);
  g_bar->add_button("go-previous", "Previous match (Shift+F3)", cmd_cb, (void *)FIND_PREV);
  g_bar->add_button("go-next", "Next match (F3)", cmd_cb, (void *)FIND_NEXT);
  g_found = new Fl_Box(0, 6, 90, 28, "");
  g_found->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_found->labelsize(12);
  g_found->labelcolor(fld::palette().fg_dim);
  g_bar->add_widget(g_found);
  g_bar->end();
  g_thumbs = new PageStrip(0, TB, 180, H - TB, true);
  g_thumbs->on_page = [](int p) { goto_page(p); };
  g_view = new PageStrip(180, TB, W - 180, H - TB, false);
  g_view->on_page = page_changed;
  g_view->on_zoom = zoom_by;
  g_win->resizable(g_view);
  g_win->end();
  g_win->size_range(500, 360);
  fld::set_window_icon(g_win, "fl-docs");
  update_zoom_label();
  g_win->show(); // not show(argc, argv): it resets the theme colors
  if (!file.empty() && open_file(file) && start_page > 0) goto_page(start_page);
  g_view->take_focus();
  return Fl::run();
}
