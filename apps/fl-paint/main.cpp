// fl-paint: a simple raster drawing program. Brush, eraser, lines,
// rectangles and ellipses, fill, text, a rectangular selection (copy, cut,
// paste, crop), a colour picker, a few layers, undo, zoom; PNG, JPEG, BMP and
// GIF in, PNG out.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Color_Chooser.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Image_Surface.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_PNG_Image.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Shared_Image.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "paint_util.h"

using paint::Canvas;
using paint::Color;

namespace {

enum Tool { BRUSH, ERASER, LINE, RECT, ELLIPSE, FILL, TEXT, SELECT, PICKER, N_TOOLS };
const char *kToolNames[] = {"Brush (B)", "Eraser (E)", "Line (L)", "Rectangle (R)", "Ellipse (O)",
                            "Fill (F)", "Text (T)", "Select (S)", "Pick a colour (I)"};
const char *kToolIcons[] = {"draw-brush", "draw-eraser", "draw-line", "draw-rectangle", "draw-ellipse",
                            "color-fill", "draw-text", "edit-select", "color-picker"};
const char *kToolGlyphs[] = {"🖌", "⌫", "╱", "▭", "◯", "🪣", "T", "⬚", "💧"};

struct Doc {
  std::vector<paint::Layer> layers;
  int active = 0;
  std::string path;
  bool dirty = false;
  std::vector<std::pair<std::vector<paint::Layer>, int>> undo, redo;
} D;

Tool g_tool = BRUSH;
Color g_color = paint::rgba(0x20, 0x20, 0x20);
double g_size = 6;
bool g_fill_shapes = false;
paint::Rect g_sel; // the selection, empty when none
Canvas g_clip;     // copied pixels

Fl_Double_Window *g_win;
Fl_Scroll *g_scroll;
class CanvasView;
CanvasView *g_view;
Fl_Hold_Browser *g_layers;
Fl_Value_Slider *g_opacity;
Fl_Box *g_status;
Fl_Button *g_color_btn;
std::vector<fld::FlatButton *> g_tool_btns;
double g_zoom = 1.0;

void update_title();
void fill_layers();

Canvas &cur() { return D.layers[(size_t)D.active].c; }
int img_w() { return D.layers.empty() ? 0 : D.layers[0].c.w; }
int img_h() { return D.layers.empty() ? 0 : D.layers[0].c.h; }

void push_undo() {
  D.undo.push_back({D.layers, D.active});
  if (D.undo.size() > 25) D.undo.erase(D.undo.begin());
  D.redo.clear();
  D.dirty = true;
  update_title();
}

void status(const std::string &s) {
  static std::string keep;
  keep = s;
  g_status->label(keep.c_str());
  g_status->redraw();
}

// ---- the canvas ---------------------------------------------------------------------------

class CanvasView : public Fl_Widget {
public:
  std::vector<unsigned char> rgba; // the composite over a checkerboard
  std::unique_ptr<Fl_RGB_Image> img;
  bool stale = true;
  // a drag in progress
  bool dragging = false;
  double x0 = 0, y0 = 0, lx = 0, ly = 0;
  Canvas before; // the active layer when the drag began (shapes are drawn again on each move)

  CanvasView(int X, int Y, int W, int H) : Fl_Widget(X, Y, W, H) {}

  void changed() {
    stale = true;
    redraw();
  }

  void rebuild() {
    Canvas flat = paint::flatten(D.layers, 0);
    int W = flat.w, H = flat.h;
    rgba.resize((size_t)W * H * 4);
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++) {
        // a checkerboard behind transparency
        Color bg = ((x / 8 + y / 8) & 1) ? paint::rgba(0xcc, 0xcc, 0xcc) : paint::rgba(0xff, 0xff, 0xff);
        Color c = paint::over(flat.px[(size_t)y * W + x], bg);
        unsigned char *o = &rgba[((size_t)y * W + x) * 4];
        o[0] = (unsigned char)paint::ch_r(c);
        o[1] = (unsigned char)paint::ch_g(c);
        o[2] = (unsigned char)paint::ch_b(c);
        o[3] = 255;
      }
    img.reset(new Fl_RGB_Image(rgba.data(), W, H, 4));
    stale = false;
  }

  void draw() override {
    fl_push_clip(x(), y(), w(), h());
    fl_color(fl_darker(fld::palette().bg));
    fl_rectf(x(), y(), w(), h());
    if (stale) rebuild();
    if (img && img->w() > 0) {
      int sw = (int)std::lround(img_w() * g_zoom), sh = (int)std::lround(img_h() * g_zoom);
      img->scale(sw, sh, 0, 1);
      img->draw(x(), y());
      // the selection, marching-ants style
      if (!g_sel.empty()) {
        int sx = x() + (int)(g_sel.x * g_zoom), sy = y() + (int)(g_sel.y * g_zoom);
        int ssw = (int)std::lround(g_sel.w * g_zoom), ssh = (int)std::lround(g_sel.h * g_zoom);
        fl_color(FL_WHITE);
        fl_rect(sx, sy, ssw, ssh);
        fl_line_style(FL_DASH, 1);
        fl_color(FL_BLACK);
        fl_rect(sx, sy, ssw, ssh);
        fl_line_style(0);
      }
    }
    fl_pop_clip();
  }

  void to_image(double &ix, double &iy) {
    ix = (Fl::event_x() - x()) / g_zoom;
    iy = (Fl::event_y() - y()) / g_zoom;
  }

  void shape(double ax, double ay, double bx, double by) {
    cur() = before;
    paint::Rect r = paint::span((int)std::floor(std::min(ax, bx)), (int)std::floor(std::min(ay, by)),
                                (int)std::floor(std::max(ax, bx)), (int)std::floor(std::max(ay, by)), img_w(), img_h());
    // Shift: squares and circles
    if (Fl::event_state() & FL_SHIFT) {
      int s = std::min(r.w, r.h);
      if (bx < ax) r.x = r.x + r.w - s;
      if (by < ay) r.y = r.y + r.h - s;
      r.w = r.h = s;
    }
    if (g_tool == LINE) paint::line(cur(), ax, ay, bx, by, g_color, g_size);
    else if (g_tool == RECT) paint::rectangle(cur(), r, g_color, g_size, g_fill_shapes);
    else if (g_tool == ELLIPSE) paint::ellipse(cur(), r, g_color, g_size, g_fill_shapes);
  }

  int handle(int e) override {
    if (D.layers.empty()) return Fl_Widget::handle(e);
    double ix, iy;
    switch (e) {
      case FL_ENTER:
        fl_cursor(g_tool == SELECT ? FL_CURSOR_CROSS : FL_CURSOR_CROSS);
        return 1;
      case FL_LEAVE:
        fl_cursor(FL_CURSOR_DEFAULT);
        return 1;
      case FL_MOVE: {
        to_image(ix, iy);
        char b[64];
        snprintf(b, sizeof b, "%d, %d", (int)ix, (int)iy);
        status(b);
        return 1;
      }
      case FL_MOUSEWHEEL:
        if (Fl::event_state() & FL_CTRL) {
          extern void zoom_by(double);
          zoom_by(Fl::event_dy() < 0 ? 1.25 : 0.8);
          return 1;
        }
        return 0;
      case FL_PUSH: {
        if (Fl::event_button() != FL_LEFT_MOUSE) return 1;
        to_image(ix, iy);
        x0 = lx = ix;
        y0 = ly = iy;
        if (g_tool == PICKER) {
          Canvas flat = paint::flatten(D.layers, 0);
          Color c = flat.get((int)ix, (int)iy);
          if (paint::ch_a(c)) {
            g_color = c | 0xff000000u;
            extern void update_color_button();
            update_color_button();
          }
          return 1;
        }
        if (g_tool == SELECT) {
          g_sel = paint::Rect();
          dragging = true;
          redraw();
          return 1;
        }
        if (g_tool == TEXT) {
          extern void place_text(int, int);
          place_text((int)ix, (int)iy);
          return 1;
        }
        push_undo();
        if (g_tool == FILL) {
          paint::flood_fill(cur(), (int)ix, (int)iy, g_color, 24);
          changed();
          return 1;
        }
        before = cur();
        dragging = true;
        if (g_tool == BRUSH || g_tool == ERASER) paint::stroke(cur(), ix, iy, ix, iy, g_color, g_size, g_tool == ERASER);
        changed();
        return 1;
      }
      case FL_DRAG: {
        if (!dragging) return 1;
        to_image(ix, iy);
        if (g_tool == BRUSH || g_tool == ERASER) {
          paint::stroke(cur(), lx, ly, ix, iy, g_color, g_size, g_tool == ERASER);
        } else if (g_tool == SELECT) {
          g_sel = paint::span((int)x0, (int)y0, (int)ix, (int)iy, img_w(), img_h());
          redraw();
        } else {
          shape(x0, y0, ix, iy);
        }
        lx = ix;
        ly = iy;
        if (g_tool != SELECT) changed();
        return 1;
      }
      case FL_RELEASE:
        if (dragging && g_tool == SELECT && g_sel.w <= 1 && g_sel.h <= 1) g_sel = paint::Rect();
        dragging = false;
        before = Canvas();
        redraw();
        return 1;
      case FL_DND_ENTER: case FL_DND_DRAG: case FL_DND_RELEASE: return 1;
    }
    return Fl_Widget::handle(e);
  }
};

void relayout_view() {
  int W = std::max(1, (int)std::lround(img_w() * g_zoom)), H = std::max(1, (int)std::lround(img_h() * g_zoom));
  g_view->resize(g_scroll->x(), g_scroll->y(), W, H);
  g_scroll->scroll_to(0, 0);
  g_scroll->redraw();
  g_view->changed();
  char b[64];
  snprintf(b, sizeof b, "%d × %d  —  %d%%", img_w(), img_h(), (int)std::lround(g_zoom * 100));
  status(b);
}

void zoom_by(double f) {
  g_zoom = std::max(0.1, std::min(16.0, g_zoom * f));
  relayout_view();
}

void zoom_fit() {
  if (!img_w()) return;
  double z = std::min((g_scroll->w() - 20) / (double)img_w(), (g_scroll->h() - 20) / (double)img_h());
  g_zoom = std::max(0.1, std::min(1.0, z));
  relayout_view();
}

void update_title() {
  std::string n = D.path.empty() ? "Untitled" : fld::basename(D.path);
  static std::string t;
  t = (D.dirty ? "*" : "") + n + " – Paint";
  g_win->label(t.c_str());
}

void update_color_button() {
  g_color_btn->color(fl_rgb_color((uchar)paint::ch_r(g_color), (uchar)paint::ch_g(g_color), (uchar)paint::ch_b(g_color)));
  g_color_btn->redraw();
}

void set_tool(Tool t) {
  g_tool = t;
  for (int i = 0; i < N_TOOLS; i++) g_tool_btns[(size_t)i]->value(i == t);
  status(kToolNames[t]);
}

// ---- documents ----------------------------------------------------------------------------

void new_doc(int W, int H, bool white) {
  D = Doc();
  paint::Layer l;
  l.name = "Background";
  l.c = Canvas(W, H, white ? paint::rgba(255, 255, 255) : 0);
  D.layers.push_back(l);
  g_sel = paint::Rect();
  fill_layers();
  update_title();
  zoom_fit();
}

bool load(const std::string &path) {
  Fl_Shared_Image *si = Fl_Shared_Image::get(path.c_str());
  if (!si || si->fail() || si->w() <= 0) {
    if (si) si->release();
    fl_alert("Cannot open %s", path.c_str());
    return false;
  }
  int W = si->w(), H = si->h(), d = si->d();
  const unsigned char *data = (const unsigned char *)si->data()[0];
  int ld = si->ld() ? si->ld() : W * d;
  Canvas c(W, H);
  for (int y = 0; y < H; y++)
    for (int x = 0; x < W; x++) {
      const unsigned char *p = data + (size_t)y * ld + (size_t)x * d;
      int r = p[0], g = d >= 3 ? p[1] : p[0], b = d >= 3 ? p[2] : p[0];
      int a = d == 4 ? p[3] : d == 2 ? p[1] : 255;
      c.px[(size_t)y * W + x] = paint::rgba(r, g, b, a);
    }
  si->release();
  D = Doc();
  paint::Layer l;
  l.name = "Background";
  l.c = c;
  D.layers.push_back(l);
  D.path = path;
  g_sel = paint::Rect();
  fill_layers();
  update_title();
  zoom_fit();
  return true;
}

bool write_png(const Canvas &c, const std::string &path) {
  std::vector<unsigned char> buf((size_t)c.w * c.h * 4);
  for (size_t i = 0; i < c.px.size(); i++) {
    buf[i * 4] = (unsigned char)paint::ch_r(c.px[i]);
    buf[i * 4 + 1] = (unsigned char)paint::ch_g(c.px[i]);
    buf[i * 4 + 2] = (unsigned char)paint::ch_b(c.px[i]);
    buf[i * 4 + 3] = (unsigned char)paint::ch_a(c.px[i]);
  }
  return fl_write_png(path.c_str(), buf.data(), c.w, c.h, 4) == 0;
}

bool save_as();

bool save() {
  if (D.path.empty() || fld::extension(D.path) != "png") return save_as();
  if (!write_png(paint::flatten(D.layers, 0), D.path)) {
    fl_alert("Cannot write %s", D.path.c_str());
    return false;
  }
  D.dirty = false;
  update_title();
  if (D.layers.size() > 1) status("Saved (the layers are merged in the PNG).");
  return true;
}

bool save_as() {
  Fl_Native_File_Chooser fc;
  fc.title("Save Picture");
  fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  fc.filter("PNG image\t*.png");
  std::string dir = fld::user_dir("PICTURES");
  if (!dir.empty()) fc.directory(dir.c_str());
  fc.preset_file(D.path.empty() ? "Untitled.png" : (fld::basename(D.path).substr(0, fld::basename(D.path).rfind('.')) + ".png").c_str());
  fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
  if (fc.show() != 0) return false;
  std::string p = fc.filename();
  if (fld::extension(p) != "png") p += ".png";
  D.path = p;
  return save();
}

bool confirm_discard() {
  if (!D.dirty) return true;
  int r = fl_choice("Save the changes to %s?", "Cancel", "Save", "Discard",
                    D.path.empty() ? "the picture" : fld::basename(D.path).c_str());
  if (r == 0) return false;
  if (r == 1) return save();
  return true;
}

void open_dialog() {
  if (!confirm_discard()) return;
  Fl_Native_File_Chooser fc;
  fc.title("Open Picture");
  fc.filter("Pictures\t*.{png,jpg,jpeg,bmp,gif}");
  std::string dir = fld::user_dir("PICTURES");
  if (!dir.empty()) fc.directory(dir.c_str());
  if (fc.show() != 0) return;
  load(fc.filename());
}

// ---- edits --------------------------------------------------------------------------------

void undo() {
  if (D.undo.empty()) return;
  D.redo.push_back({D.layers, D.active});
  D.layers = D.undo.back().first;
  D.active = D.undo.back().second;
  D.undo.pop_back();
  D.dirty = true;
  fill_layers();
  relayout_view();
  update_title();
}

void redo() {
  if (D.redo.empty()) return;
  D.undo.push_back({D.layers, D.active});
  D.layers = D.redo.back().first;
  D.active = D.redo.back().second;
  D.redo.pop_back();
  fill_layers();
  relayout_view();
  update_title();
}

void copy_sel(bool cut) {
  paint::Rect r = g_sel.empty() ? paint::Rect{0, 0, img_w(), img_h()} : g_sel;
  g_clip = paint::crop(cur(), r);
  // also as a PNG on the system clipboard (wl-copy)
  std::string tmp = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-paint-copy.png";
  if (!fld::which("wl-copy").empty() && write_png(g_clip, tmp))
    fld::spawn_shell("wl-copy --type image/png < " + fld::shell_quote(tmp) + "; rm -f " + fld::shell_quote(tmp));
  if (cut) {
    push_undo();
    for (int y = r.y; y < r.y + r.h; y++)
      for (int x = r.x; x < r.x + r.w; x++) cur().set(x, y, 0);
    g_view->changed();
  }
}

void paste() {
  if (g_clip.w == 0) return;
  push_undo();
  // a new layer with the pasted pixels where the selection is
  paint::Layer l;
  l.name = "Pasted";
  l.c = Canvas(img_w(), img_h());
  int ox = g_sel.empty() ? 0 : g_sel.x, oy = g_sel.empty() ? 0 : g_sel.y;
  paint::draw_over(l.c, g_clip, ox, oy);
  D.layers.insert(D.layers.begin() + D.active + 1, l);
  D.active++;
  fill_layers();
  g_view->changed();
}

void delete_sel() {
  if (g_sel.empty()) return;
  push_undo();
  for (int y = g_sel.y; y < g_sel.y + g_sel.h; y++)
    for (int x = g_sel.x; x < g_sel.x + g_sel.w; x++) cur().set(x, y, 0);
  g_view->changed();
}

void crop_to_sel() {
  if (g_sel.empty()) return;
  push_undo();
  for (auto &l : D.layers) l.c = paint::crop(l.c, g_sel);
  g_sel = paint::Rect();
  relayout_view();
}

void all_layers(Canvas (*f)(const Canvas &)) {
  push_undo();
  for (auto &l : D.layers) l.c = f(l.c);
  g_sel = paint::Rect();
  relayout_view();
}

bool ask_size(const char *title, int &W, int &H, bool *keep_ratio) {
  Fl_Double_Window d(320, keep_ratio ? 170 : 140, title);
  d.set_modal();
  Fl_Spinner sw(110, 16, 100, 28, "Width:");
  Fl_Spinner sh(110, 52, 100, 28, "Height:");
  for (Fl_Spinner *s : {&sw, &sh}) {
    s->range(1, 16384);
    s->step(1);
  }
  sw.value(W);
  sh.value(H);
  Fl_Check_Button *keep = keep_ratio ? new Fl_Check_Button(110, 86, 200, 26, "Keep the proportions") : nullptr;
  if (keep) keep->value(1);
  struct Ctx {
    Fl_Spinner *w, *h;
    Fl_Check_Button *k;
    double ratio;
    bool busy;
  } ctx{&sw, &sh, keep, H > 0 ? (double)W / H : 1, false};
  sw.callback([](Fl_Widget *, void *v) {
    Ctx *c = (Ctx *)v;
    if (c->k && c->k->value() && !c->busy) {
      c->busy = true;
      c->h->value(std::max(1.0, std::round(c->w->value() / c->ratio)));
      c->busy = false;
    }
  }, &ctx);
  sh.callback([](Fl_Widget *, void *v) {
    Ctx *c = (Ctx *)v;
    if (c->k && c->k->value() && !c->busy) {
      c->busy = true;
      c->w->value(std::max(1.0, std::round(c->h->value() * c->ratio)));
      c->busy = false;
    }
  }, &ctx);
  Fl_Button cancel(96, d.h() - 42, 100, 30, "Cancel");
  Fl_Return_Button ok(204, d.h() - 42, 100, 30, "OK");
  int result = 0;
  cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 2; }, &result);
  ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
  d.end();
  d.show();
  while (!result && d.shown()) Fl::wait();
  W = (int)sw.value();
  H = (int)sh.value();
  if (keep_ratio) *keep_ratio = keep && keep->value();
  delete keep;
  return result == 1;
}

void place_text(int ix, int iy) {
  const char *t = fl_input("Text:", "");
  if (!t || !*t) return;
  int size = std::max(8, (int)std::lround(g_size * 4));
  fl_font(FL_HELVETICA, size);
  int tw = 0, th = 0;
  fl_measure(t, tw, th, 0);
  tw += 4;
  th += 4;
  // black text on white: the darkness is the coverage
  Fl_Image_Surface surf(tw, th);
  Fl_Surface_Device::push_current(&surf);
  fl_color(FL_WHITE);
  fl_rectf(0, 0, tw, th);
  fl_color(FL_BLACK);
  fl_font(FL_HELVETICA, size);
  fl_draw(t, 2, 2, tw - 4, th - 4, FL_ALIGN_TOP_LEFT);
  Fl_RGB_Image *img = surf.image();
  Fl_Surface_Device::pop_current();
  if (!img) return;
  push_undo();
  const unsigned char *p = (const unsigned char *)img->data()[0];
  int d = img->d(), ld = img->ld() ? img->ld() : img->w() * d;
  for (int y = 0; y < img->h(); y++)
    for (int x = 0; x < img->w(); x++) {
      int lum = p[(size_t)y * ld + (size_t)x * d];
      if (lum < 250) cur().blend(ix + x, iy + y, g_color, (255 - lum) / 255.0);
    }
  delete img;
  g_view->changed();
}

// ---- layers -------------------------------------------------------------------------------

void fill_layers() {
  g_layers->clear();
  // top layer first
  for (int i = (int)D.layers.size() - 1; i >= 0; i--) {
    const paint::Layer &l = D.layers[(size_t)i];
    g_layers->add(((l.visible ? "👁 " : "    ") + l.name).c_str());
  }
  g_layers->select((int)D.layers.size() - D.active);
  if (!D.layers.empty()) g_opacity->value(D.layers[(size_t)D.active].opacity * 100);
}

void layer_action(const std::string &a) {
  if (a == "add") {
    push_undo();
    paint::Layer l;
    l.name = "Layer " + std::to_string(D.layers.size() + 1);
    l.c = Canvas(img_w(), img_h());
    D.layers.insert(D.layers.begin() + D.active + 1, l);
    D.active++;
  } else if (a == "delete" && D.layers.size() > 1) {
    push_undo();
    D.layers.erase(D.layers.begin() + D.active);
    D.active = std::max(0, D.active - 1);
  } else if (a == "up" && D.active + 1 < (int)D.layers.size()) {
    push_undo();
    std::swap(D.layers[(size_t)D.active], D.layers[(size_t)D.active + 1]);
    D.active++;
  } else if (a == "down" && D.active > 0) {
    push_undo();
    std::swap(D.layers[(size_t)D.active], D.layers[(size_t)D.active - 1]);
    D.active--;
  } else if (a == "merge" && D.active > 0) {
    push_undo();
    paint::Layer &top = D.layers[(size_t)D.active];
    paint::draw_over(D.layers[(size_t)D.active - 1].c, top.c, 0, 0, top.visible ? top.opacity : 0);
    D.layers.erase(D.layers.begin() + D.active);
    D.active--;
  } else if (a == "visible") {
    D.layers[(size_t)D.active].visible = !D.layers[(size_t)D.active].visible;
    D.dirty = true;
  }
  fill_layers();
  g_view->changed();
}

// ---- menus --------------------------------------------------------------------------------

void menu_cb(Fl_Widget *, void *v) {
  std::string a = (const char *)v;
  if (a == "new") {
    if (!confirm_discard()) return;
    int W = 1024, H = 768;
    if (ask_size("New Picture", W, H, nullptr)) new_doc(W, H, true);
  } else if (a == "open") open_dialog();
  else if (a == "save") save();
  else if (a == "saveas") save_as();
  else if (a == "quit") {
    if (confirm_discard()) g_win->hide();
  } else if (a == "undo") undo();
  else if (a == "redo") redo();
  else if (a == "copy") copy_sel(false);
  else if (a == "cut") copy_sel(true);
  else if (a == "paste") paste();
  else if (a == "delete") delete_sel();
  else if (a == "selall") {
    g_sel = {0, 0, img_w(), img_h()};
    g_view->redraw();
  } else if (a == "selnone") {
    g_sel = paint::Rect();
    g_view->redraw();
  } else if (a == "crop") crop_to_sel();
  else if (a == "resize") {
    int W = img_w(), H = img_h();
    bool keep;
    if (!ask_size("Resize Picture", W, H, &keep)) return;
    push_undo();
    for (auto &l : D.layers) l.c = paint::scale(l.c, W, H);
    relayout_view();
  } else if (a == "canvas") {
    int W = img_w(), H = img_h();
    if (!ask_size("Canvas Size", W, H, nullptr)) return;
    push_undo();
    for (size_t i = 0; i < D.layers.size(); i++)
      D.layers[i].c = paint::resize_canvas(D.layers[i].c, W, H, 0, 0, i == 0 ? paint::rgba(255, 255, 255) : 0);
    relayout_view();
  } else if (a == "rotcw") all_layers([](const Canvas &c) { return paint::rotate90(c, true); });
  else if (a == "rotccw") all_layers([](const Canvas &c) { return paint::rotate90(c, false); });
  else if (a == "fliph") all_layers([](const Canvas &c) { return paint::flip(c, true); });
  else if (a == "flipv") all_layers([](const Canvas &c) { return paint::flip(c, false); });
  else if (a == "zoomin") zoom_by(1.25);
  else if (a == "zoomout") zoom_by(0.8);
  else if (a == "zoom1") {
    g_zoom = 1;
    relayout_view();
  } else if (a == "zoomfit") zoom_fit();
  else if (a == "about") fld::about_dialog("Paint", "fl-paint", "A simple drawing program for fldesktop.");
}

int keys(int e) {
  if (e != FL_SHORTCUT || Fl::focus() && Fl::focus()->top_window() != g_win) return 0;
  if (Fl::event_state() & (FL_CTRL | FL_ALT)) return 0;
  switch (Fl::event_key()) {
    case 'b': set_tool(BRUSH); return 1;
    case 'e': set_tool(ERASER); return 1;
    case 'l': set_tool(LINE); return 1;
    case 'r': set_tool(RECT); return 1;
    case 'o': set_tool(ELLIPSE); return 1;
    case 'f': set_tool(FILL); return 1;
    case 't': set_tool(TEXT); return 1;
    case 's': set_tool(SELECT); return 1;
    case 'i': set_tool(PICKER); return 1;
    case '[': g_size = std::max(1.0, g_size - 1); status("Size " + std::to_string((int)g_size)); return 1;
    case ']': g_size = std::min(200.0, g_size + 1); status("Size " + std::to_string((int)g_size)); return 1;
    case FL_Delete: delete_sel(); return 1;
    case FL_Escape:
      g_sel = paint::Rect();
      g_view->redraw();
      return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  fld::init_app("fl-paint", argc, argv);
  fl_register_images();
  std::string file;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-paint [PICTURE]\n");
      return 0;
    }
    if (a[0] != '-') file = fld::abs_path(a);
  }
  const int W = 1100, H = 740, TOOLW = 92, SIDEW = 200;
  g_win = new Fl_Double_Window(W, H, "Paint");
  Fl_Menu_Bar *mb = new Fl_Menu_Bar(0, 0, W, 28);
  struct M {
    const char *path;
    int key;
    const char *act;
    int flags;
  } items[] = {{"&File/&New…", FL_CTRL + 'n', "new", 0},
               {"&File/&Open…", FL_CTRL + 'o', "open", 0},
               {"&File/&Save", FL_CTRL + 's', "save", 0},
               {"&File/Save &As…", FL_CTRL + FL_SHIFT + 's', "saveas", FL_MENU_DIVIDER},
               {"&File/&Quit", FL_CTRL + 'q', "quit", 0},
               {"&Edit/&Undo", FL_CTRL + 'z', "undo", 0},
               {"&Edit/&Redo", FL_CTRL + FL_SHIFT + 'z', "redo", FL_MENU_DIVIDER},
               {"&Edit/Cu&t", FL_CTRL + 'x', "cut", 0},
               {"&Edit/&Copy", FL_CTRL + 'c', "copy", 0},
               {"&Edit/&Paste", FL_CTRL + 'v', "paste", 0},
               {"&Edit/&Delete", 0, "delete", FL_MENU_DIVIDER},
               {"&Edit/Select &All", FL_CTRL + 'a', "selall", 0},
               {"&Edit/Select &None", FL_CTRL + FL_SHIFT + 'a', "selnone", 0},
               {"&Image/&Crop to Selection", FL_CTRL + FL_SHIFT + 'x', "crop", FL_MENU_DIVIDER},
               {"&Image/&Resize…", FL_CTRL + 'r', "resize", 0},
               {"&Image/Canvas &Size…", 0, "canvas", FL_MENU_DIVIDER},
               {"&Image/Rotate &Right", 0, "rotcw", 0},
               {"&Image/Rotate &Left", 0, "rotccw", 0},
               {"&Image/Flip &Horizontally", 0, "fliph", 0},
               {"&Image/Flip &Vertically", 0, "flipv", 0},
               {"&View/Zoom &In", FL_CTRL + '=', "zoomin", 0},
               {"&View/Zoom &Out", FL_CTRL + '-', "zoomout", 0},
               {"&View/&Actual Size", FL_CTRL + '0', "zoom1", 0},
               {"&View/&Fit", FL_CTRL + '9', "zoomfit", 0},
               {"&Help/&About", 0, "about", 0}};
  for (auto &m : items) mb->add(m.path, m.key, menu_cb, (void *)m.act, m.flags);

  // tools
  Fl_Group *tools = new Fl_Group(0, 28, TOOLW, H - 54);
  tools->box(FL_FLAT_BOX);
  for (int i = 0; i < N_TOOLS; i++) {
    fld::FlatButton *b = new fld::FlatButton(6 + (i % 2) * 42, 36 + (i / 2) * 42, 40, 40);
    if (fld::icon(kToolIcons[i], 22)) b->icon_name(kToolIcons[i], 22);
    else b->copy_label(kToolGlyphs[i]);
    b->tooltip(kToolNames[i]);
    b->type(FL_TOGGLE_BUTTON);
    b->callback([](Fl_Widget *, void *t) { set_tool((Tool)(long)t); }, (void *)(long)i);
    g_tool_btns.push_back(b);
  }
  int ty = 36 + ((N_TOOLS + 1) / 2) * 42 + 8;
  g_color_btn = new Fl_Button(6, ty, 82, 32);
  g_color_btn->box(FL_BORDER_BOX);
  g_color_btn->tooltip("The colour (click to choose)");
  g_color_btn->callback([](Fl_Widget *, void *) {
    uchar r = (uchar)paint::ch_r(g_color), g = (uchar)paint::ch_g(g_color), b = (uchar)paint::ch_b(g_color);
    if (fl_color_chooser("Colour", r, g, b, 1)) {
      g_color = paint::rgba(r, g, b);
      update_color_button();
    }
  });
  ty += 38;
  static const Color swatches[] = {paint::rgba(0, 0, 0), paint::rgba(255, 255, 255), paint::rgba(128, 128, 128),
                                   paint::rgba(220, 40, 40), paint::rgba(245, 150, 30), paint::rgba(250, 215, 40),
                                   paint::rgba(60, 170, 70), paint::rgba(40, 120, 220), paint::rgba(140, 70, 200),
                                   paint::rgba(120, 70, 30), paint::rgba(240, 130, 180), paint::rgba(40, 200, 200)};
  for (int i = 0; i < 12; i++) {
    Fl_Button *s = new Fl_Button(6 + (i % 4) * 21, ty + (i / 4) * 21, 19, 19);
    Color c = swatches[i];
    s->box(FL_BORDER_BOX);
    s->color(fl_rgb_color((uchar)paint::ch_r(c), (uchar)paint::ch_g(c), (uchar)paint::ch_b(c)));
    s->callback([](Fl_Widget *, void *v) {
      g_color = (Color)(uintptr_t)v;
      update_color_button();
    }, (void *)(uintptr_t)c);
  }
  ty += 3 * 21 + 10;
  Fl_Value_Slider *size = new Fl_Value_Slider(6, ty + 18, 82, 22, "Size");
  size->align(FL_ALIGN_TOP_LEFT);
  size->type(FL_HOR_NICE_SLIDER);
  size->bounds(1, 100);
  size->step(1);
  size->value(g_size);
  size->callback([](Fl_Widget *w, void *) { g_size = ((Fl_Value_Slider *)w)->value(); });
  ty += 48;
  Fl_Check_Button *fillck = new Fl_Check_Button(6, ty, 84, 24, "Filled");
  fillck->tooltip("Rectangles and ellipses filled, not outlined");
  fillck->callback([](Fl_Widget *w, void *) { g_fill_shapes = ((Fl_Check_Button *)w)->value(); });
  tools->end();
  tools->resizable(nullptr);

  g_scroll = new Fl_Scroll(TOOLW, 28, W - TOOLW - SIDEW, H - 54);
  g_scroll->box(FL_FLAT_BOX);
  g_scroll->color(fl_darker(fld::palette().bg));
  g_view = new CanvasView(TOOLW, 28, 10, 10);
  g_scroll->end();

  Fl_Group *side = new Fl_Group(W - SIDEW, 28, SIDEW, H - 54);
  Fl_Box *lh = new Fl_Box(W - SIDEW + 6, 32, SIDEW - 12, 22, "Layers");
  lh->labelfont(FL_HELVETICA_BOLD);
  lh->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_layers = new Fl_Hold_Browser(W - SIDEW + 6, 56, SIDEW - 12, 220);
  g_layers->format_char(0);
  g_layers->callback([](Fl_Widget *, void *) {
    int i = g_layers->value();
    if (i <= 0) return;
    D.active = (int)D.layers.size() - i;
    if (Fl::event_clicks() && Fl::event() == FL_RELEASE) layer_action("visible");
    else fill_layers();
  });
  int lx = W - SIDEW + 6;
  struct LB {
    const char *icon, *glyph, *tip, *act;
  } lbs[] = {{"list-add", "+", "New layer", "add"},
             {"list-remove", "−", "Delete the layer", "delete"},
             {"go-up", "↑", "Move up", "up"},
             {"go-down", "↓", "Move down", "down"},
             {"go-bottom", "⤓", "Merge into the layer below", "merge"}};
  for (auto &b : lbs) {
    fld::FlatButton *fb = new fld::FlatButton(lx, 280, 34, 30);
    if (fld::icon(b.icon, 18)) fb->icon_name(b.icon, 18);
    else fb->copy_label(b.glyph);
    fb->tooltip(b.tip);
    fb->callback([](Fl_Widget *, void *a) { layer_action((const char *)a); }, (void *)b.act);
    lx += 36;
  }
  g_opacity = new Fl_Value_Slider(W - SIDEW + 6, 336, SIDEW - 12, 22, "Opacity %");
  g_opacity->align(FL_ALIGN_TOP_LEFT);
  g_opacity->type(FL_HOR_NICE_SLIDER);
  g_opacity->bounds(0, 100);
  g_opacity->step(1);
  g_opacity->value(100);
  g_opacity->callback([](Fl_Widget *w, void *) {
    D.layers[(size_t)D.active].opacity = ((Fl_Value_Slider *)w)->value() / 100.0;
    D.dirty = true;
    g_view->changed();
  });
  Fl_Box *hint = new Fl_Box(W - SIDEW + 6, 372, SIDEW - 12, 120,
                            "Double-click a layer to hide or show it. Ctrl+wheel zooms; Shift draws squares "
                            "and circles; [ and ] change the size.");
  hint->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  hint->labelsize(11);
  hint->labelcolor(fld::palette().fg_dim);
  side->end();
  side->resizable(nullptr);

  g_status = new Fl_Box(8, H - 26, W - 16, 24);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->end();
  g_win->resizable(g_scroll);
  g_win->size_range(700, 500);
  g_win->callback([](Fl_Widget *w, void *) {
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
    if (confirm_discard()) w->hide();
  });
  fld::set_window_icon(g_win, "fl-paint");
  Fl::add_handler(keys);
  update_color_button();
  set_tool(BRUSH);
  g_win->show();
  if (file.empty() || !load(file)) new_doc(1024, 768, true);
  return Fl::run();
}
