// fl-snip: the editor (pen, highlighter, shapes, text, pixelate, crop)
#include "fld/i18n.h"
#include "snip_app.h"

// ---------------------------------------------------------------------------
// The editor

enum class Tool { Pen, Highlighter, Line, Arrow, Box, Ellipse, Text, Pixelate, Crop, Eraser };

struct Shape {
  Tool tool;
  Fl_Color color;
  int size;
  std::vector<Point> pts; // image pixels; two points for lines, boxes, crops
  std::string text;
};

static const Fl_Color kColors[] = {0xE01B2400, 0xF6D32D00, 0x33D17A00, 0x3584E400, 0x9141AC00, 0xFFFFFF00,
                                   0x00000000};
static const char *kColorNames[] = {"Red", "Yellow", "Green", "Blue", "Purple", "White", "Black"};

class EditorWindow;
std::vector<EditorWindow *> g_editors;

class Canvas : public Fl_Widget {
public:
  EditorWindow *ed;
  double zoom = 1;
  bool fit = true;
  Canvas(int x, int y, int w, int h, EditorWindow *e) : Fl_Widget(x, y, w, h), ed(e) {}
  void draw() override;
  int handle(int e) override;
  // image pixel <-> widget coordinates
  double ox() const;
  double oy() const;
  Point to_image(int x, int y) const;
};

class EditorWindow : public Fl_Double_Window {
public:
  Image base;      // the snip as taken
  Image work;      // base with the pixel shapes (highlighter, pixelate) applied
  std::vector<Shape> shapes;
  std::vector<std::vector<Shape>> undo_stack, redo_stack;
  std::unique_ptr<Fl_RGB_Image> work_img;
  std::string path; // where it was saved last
  bool dirty = false;
  Tool tool = Tool::Pen;
  int color = 0, pen_size = 4;
  Shape *live = nullptr; // being drawn
  Shape live_shape;
  Canvas *canvas;
  fld::Toolbar *bar;
  std::vector<GlyphButton *> tool_btns;
  Fl_Menu_Button *color_btn;
  Fl_Choice *size_choice;
  fld::FlatButton *undo_btn, *redo_btn;
  Fl_Box *status;

  explicit EditorWindow(Image img) : Fl_Double_Window(900, 640, "Snip"), base(std::move(img)) {
    const fld::Palette &p = fld::palette();
    int sw, sh, sx, sy;
    Fl::screen_work_area(sx, sy, sw, sh);
    int W = std::min(sw - 40, std::max(880, base.w + 40)), H = std::min(sh - 40, std::max(480, base.h + 120));
    size(W, H);
    const int TB = 44, SB = 24;
    bar = new fld::Toolbar(0, 0, W, TB);
    bar->color(p.bg2);
    bar->add_button("document-new", "New snip (Ctrl+N)", [](Fl_Widget *w, void *) { new_snip(S.mode, 0); }, nullptr,
                    "New");
    bar->add_separator();
    static const struct { Tool t; Glyph g; const char *tip; } tools[] = {
        {Tool::Pen, Glyph::Pen, "Pen (P)"},
        {Tool::Highlighter, Glyph::Highlighter, "Highlighter (H)"},
        {Tool::Line, Glyph::Line, "Line (L)"},
        {Tool::Arrow, Glyph::Arrow, "Arrow (A)"},
        {Tool::Box, Glyph::Box, "Rectangle (R)"},
        {Tool::Ellipse, Glyph::Ellipse, "Ellipse (E)"},
        {Tool::Text, Glyph::Text, "Text (T)"},
        {Tool::Pixelate, Glyph::Pixelate, "Pixelate, to hide something (X)"},
        {Tool::Crop, Glyph::Crop, "Crop (C)"},
        {Tool::Eraser, Glyph::Eraser, "Eraser: click a drawing to remove it (D)"}};
    for (auto &t : tools) {
      auto *b = new GlyphButton(bar->next_x(), 4, 36, 36, t.g, t.tip);
      b->color(p.bg2);
      b->callback([](Fl_Widget *w, void *d) { ((EditorWindow *)w->window())->set_tool((Tool)(long)d); },
                  (void *)(long)t.t);
      bar->add_widget(b);
      tool_btns.push_back(b);
    }
    bar->add_separator();
    color_btn = new Fl_Menu_Button(bar->next_x(), 8, 44, 28);
    color_btn->tooltip("Color");
    for (size_t i = 0; i < sizeof kColors / sizeof kColors[0]; i++)
      color_btn->add(kColorNames[i], 0, [](Fl_Widget *w, void *d) {
        EditorWindow *e = (EditorWindow *)w->window();
        e->color = (int)(long)d;
        e->color_btn->color(kColors[e->color]);
        e->color_btn->redraw();
      }, (void *)(long)i);
    color_btn->color(kColors[0]);
    color_btn->box(FL_ROUNDED_BOX);
    bar->add_widget(color_btn);
    size_choice = new Fl_Choice(bar->next_x() + 4, 8, 82, 28);
    size_choice->add("Thin");
    size_choice->add("Medium");
    size_choice->add("Thick");
    size_choice->value(1);
    size_choice->tooltip("Line width");
    size_choice->callback([](Fl_Widget *w, void *) {
      EditorWindow *e = (EditorWindow *)w->window();
      static const int sizes[] = {2, 4, 8};
      e->pen_size = sizes[((Fl_Choice *)w)->value()];
    });
    bar->add_widget(size_choice);
    bar->add_separator();
    undo_btn = bar->add_button("edit-undo", "Undo (Ctrl+Z)", [](Fl_Widget *w, void *) {
      ((EditorWindow *)w->window())->undo();
    }, nullptr);
    redo_btn = bar->add_button("edit-redo", "Redo (Ctrl+Shift+Z)", [](Fl_Widget *w, void *) {
      ((EditorWindow *)w->window())->redo();
    }, nullptr);
    bar->add_separator();
    bar->add_button("edit-copy", "Copy (Ctrl+C)", [](Fl_Widget *w, void *) { ((EditorWindow *)w->window())->copy(); },
                    nullptr);
    bar->add_button("document-save", "Save (Ctrl+S)", [](Fl_Widget *w, void *) { ((EditorWindow *)w->window())->save(); },
                    nullptr);
    bar->add_button("document-save-as", "Save As (Ctrl+Shift+S)", [](Fl_Widget *w, void *) {
      ((EditorWindow *)w->window())->save_as();
    }, nullptr);
    fld::FlatButton *more = bar->add_button("open-menu", "More", [](Fl_Widget *w, void *) {
      ((EditorWindow *)w->window())->more_menu(w);
    }, nullptr);
    if (!fld::icon("open-menu", 20)) more->label("…");
    bar->end();
    canvas = new Canvas(0, TB, W, H - TB - SB, this);
    status = new Fl_Box(0, H - SB, W, SB);
    status->box(FL_FLAT_BOX);
    status->color(p.bg2);
    status->labelsize(12);
    status->labelcolor(p.fg_dim);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    end();
    resizable(canvas);
    size_range(560, 360);
    fld::set_window_icon(this, "applets-screenshooter");
    callback([](Fl_Widget *w, void *) { ((EditorWindow *)w)->close(); });
    set_tool(Tool::Pen);
    rebuild();
    g_editors.push_back(this);
    fld::set_context_menu(canvas, [this](fld::Menu &m) { context_menu(m); }, false);
  }

  ~EditorWindow() override { g_editors.erase(std::remove(g_editors.begin(), g_editors.end(), this), g_editors.end()); }

  // the visible part: the last crop, or all
  Rect view_rect() const {
    Rect r{0, 0, base.w, base.h};
    for (auto &s : shapes)
      if (s.tool == Tool::Crop && s.pts.size() == 2)
        r = snip::clip(snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second), base.w, base.h);
    return r;
  }

  void set_tool(Tool t) {
    tool = t;
    for (size_t i = 0; i < tool_btns.size(); i++) tool_btns[i]->active_look((int)i == (int)t);
    canvas->window() ? canvas->window()->cursor(t == Tool::Text ? FL_CURSOR_INSERT : FL_CURSOR_CROSS) : (void)0;
  }

  // work = base + the highlighter and pixelate shapes; called after changes
  void rebuild() {
    work = base;
    for (auto &s : shapes) apply_pixels(work, s);
    work_img = rgb_image(work);
    update_status();
    canvas->redraw();
    undo_stack.empty() ? undo_btn->deactivate() : undo_btn->activate();
    redo_stack.empty() ? redo_btn->deactivate() : redo_btn->activate();
    std::string t = (path.empty() ? std::string("Snip") : fld::basename(path)) + (dirty ? " *" : "") + " — Snip";
    copy_label(t.c_str());
  }

  void update_status() {
    Rect r = view_rect();
    std::string s = std::to_string(r.w) + " × " + std::to_string(r.h) + " pixels";
    if (!path.empty()) s += "   " + path;
    status->copy_label(fld::escape_at(s).c_str());
  }

  static void apply_pixels(Image &img, const Shape &s) {
    if (s.tool == Tool::Pixelate && s.pts.size() == 2) {
      snip::pixelate(img, snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second), 10);
    } else if (s.tool == Tool::Highlighter && !s.pts.empty()) {
      // a marker: each channel darkened towards the color (multiply)
      uint8_t cr, cg, cb;
      Fl::get_color(s.color, cr, cg, cb);
      double rad = s.size * 2.5;
      std::vector<uint8_t> hit((size_t)img.w * img.h, 0);
      auto seg = [&](Point a, Point b) {
        int x1 = (int)std::floor(std::min(a.first, b.first) - rad), x2 = (int)std::ceil(std::max(a.first, b.first) + rad);
        int y1 = (int)std::floor(std::min(a.second, b.second) - rad), y2 = (int)std::ceil(std::max(a.second, b.second) + rad);
        double dx = b.first - a.first, dy = b.second - a.second, len2 = dx * dx + dy * dy;
        for (int y = std::max(0, y1); y <= std::min(img.h - 1, y2); y++)
          for (int x = std::max(0, x1); x <= std::min(img.w - 1, x2); x++) {
            double t = len2 > 0 ? std::max(0.0, std::min(1.0, ((x - a.first) * dx + (y - a.second) * dy) / len2)) : 0;
            double px = a.first + t * dx - x, py = a.second + t * dy - y;
            if (px * px + py * py <= rad * rad) hit[(size_t)y * img.w + x] = 1;
          }
      };
      if (s.pts.size() == 1) seg(s.pts[0], s.pts[0]);
      for (size_t i = 1; i < s.pts.size(); i++) seg(s.pts[i - 1], s.pts[i]);
      for (int y = 0; y < img.h; y++)
        for (int x = 0; x < img.w; x++)
          if (hit[(size_t)y * img.w + x]) {
            uint8_t *p = img.at(x, y);
            p[0] = (uint8_t)(p[0] * cr / 255);
            p[1] = (uint8_t)(p[1] * cg / 255);
            p[2] = (uint8_t)(p[2] * cb / 255);
          }
    }
  }

  // the vector shapes, drawn with FLTK at offset ox, oy and zoom z
  static void draw_shape(const Shape &s, double ox, double oy, double z) {
    auto X = [&](double v) { return ox + v * z; };
    auto Y = [&](double v) { return oy + v * z; };
    fl_color(s.color);
    int lw = std::max(1, (int)lround(s.size * z));
    switch (s.tool) {
      case Tool::Pen:
        fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, lw);
        if (s.pts.size() == 1) {
          fl_pie((int)(X(s.pts[0].first) - lw / 2.0), (int)(Y(s.pts[0].second) - lw / 2.0), lw, lw, 0, 360);
        } else {
          fl_begin_line();
          for (auto &p : s.pts) fl_vertex(X(p.first), Y(p.second));
          fl_end_line();
        }
        break;
      case Tool::Line:
      case Tool::Arrow:
        if (s.pts.size() < 2) break;
        fl_line_style(FL_SOLID | FL_CAP_ROUND, lw);
        fl_begin_line();
        fl_vertex(X(s.pts[0].first), Y(s.pts[0].second));
        fl_vertex(X(s.pts[1].first), Y(s.pts[1].second));
        fl_end_line();
        if (s.tool == Tool::Arrow) {
          fl_line_style(0);
          draw_arrow_head(X(s.pts[0].first), Y(s.pts[0].second), X(s.pts[1].first), Y(s.pts[1].second),
                          std::max(10.0, s.size * 4.0 * z));
        }
        break;
      case Tool::Box:
      case Tool::Ellipse: {
        if (s.pts.size() < 2) break;
        Rect r = snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second);
        fl_line_style(FL_SOLID, lw);
        int x = (int)X(r.x), y = (int)Y(r.y), w = (int)(r.w * z), h = (int)(r.h * z);
        if (s.tool == Tool::Box) fl_rect(x, y, w, h);
        else fl_arc(x, y, w, h, 0, 360);
        break;
      }
      case Tool::Text:
        if (s.pts.empty()) break;
        fl_font(FL_HELVETICA_BOLD, std::max(6, (int)lround(s.size * 5 * z)));
        fl_draw(s.text.c_str(), (int)X(s.pts[0].first), (int)Y(s.pts[0].second));
        break;
      default:
        break;
    }
    fl_line_style(0);
  }

  // the finished picture: work, the vector shapes on it, cropped
  Image flatten() {
    Image out = work;
    bool vec = std::any_of(shapes.begin(), shapes.end(), [](const Shape &s) {
      return s.tool == Tool::Pen || s.tool == Tool::Line || s.tool == Tool::Arrow || s.tool == Tool::Box ||
             s.tool == Tool::Ellipse || s.tool == Tool::Text;
    });
    if (vec) {
      Fl_Image_Surface surf(work.w, work.h);
      Fl_Surface_Device::push_current(&surf);
      fl_color(FL_WHITE);
      fl_rectf(0, 0, work.w, work.h);
      work_img->draw(0, 0);
      for (auto &s : shapes) draw_shape(s, 0, 0, 1);
      std::unique_ptr<Fl_RGB_Image> shot(surf.image());
      Fl_Surface_Device::pop_current();
      if (shot && shot->d() >= 3 && shot->data_w() == work.w && shot->data_h() == work.h) {
        const uint8_t *d = (const uint8_t *)shot->data()[0];
        int ld = shot->ld() ? shot->ld() : shot->data_w() * shot->d();
        for (int y = 0; y < work.h; y++)
          for (int x = 0; x < work.w; x++) {
            const uint8_t *s = d + (size_t)y * ld + (size_t)x * shot->d();
            uint8_t *o = out.at(x, y);
            o[0] = s[0];
            o[1] = s[1];
            o[2] = s[2];
          }
      }
    }
    Rect v = view_rect();
    if (v.x || v.y || v.w != out.w || v.h != out.h) out = snip::crop(out, v);
    return out;
  }

  void push_undo() {
    undo_stack.push_back(shapes);
    if (undo_stack.size() > 100) undo_stack.erase(undo_stack.begin());
    redo_stack.clear();
    dirty = true;
  }
  void undo() {
    if (undo_stack.empty()) return;
    redo_stack.push_back(shapes);
    shapes = undo_stack.back();
    undo_stack.pop_back();
    rebuild();
  }
  void redo() {
    if (redo_stack.empty()) return;
    undo_stack.push_back(shapes);
    shapes = redo_stack.back();
    redo_stack.pop_back();
    rebuild();
  }

  void add_shape(const Shape &s) {
    push_undo();
    shapes.push_back(s);
    if (s.tool == Tool::Crop) canvas->fit = true;
    rebuild();
  }

  // the topmost shape near an image point (for the eraser)
  int shape_at(Point p) const {
    for (int i = (int)shapes.size() - 1; i >= 0; i--) {
      const Shape &s = shapes[(size_t)i];
      if (s.tool == Tool::Crop) continue;
      Rect b = snip::bounds(s.pts);
      int pad = s.size * 3 + 4;
      if (s.tool == Tool::Text) {
        b.y -= s.size * 5;
        b.w = std::max(b.w, (int)s.text.size() * s.size * 3);
        b.h = s.size * 6;
      }
      if (Rect{b.x - pad, b.y - pad, b.w + 2 * pad, b.h + 2 * pad}.contains(p.first, p.second)) return i;
    }
    return -1;
  }

  void copy() {
    copy_image(flatten(), false);
    status->copy_label("Copied to the clipboard");
  }

  bool save_to(const std::string &p) {
    if (!write_png(flatten(), p)) {
      fl_alert("Could not save %s.", fld::escape_at(p).c_str());
      return false;
    }
    path = p;
    dirty = false;
    rebuild();
    return true;
  }
  bool save() {
    if (path.empty()) return save_as();
    return save_to(path);
  }
  bool save_as() {
    Fl_Native_File_Chooser fc;
    fc.title("Save Snip");
    fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
    fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
    fc.filter("PNG image\t*.png");
    std::string dir = path.empty() ? screenshots_dir() : fld::dirname(path);
    fld::mkdir_p(dir);
    fc.directory(dir.c_str());
    fc.preset_file(path.empty() ? snip::file_name(time(nullptr)).c_str() : fld::basename(path).c_str());
    if (fc.show() != 0 || !fc.filename()) return false;
    std::string p = fc.filename();
    if (fld::lower(fld::extension(p)) != "png") p += ".png";
    return save_to(p);
  }

  // a file of the picture as it is now (saved, or a temporary one)
  std::string current_file() {
    if (!path.empty() && !dirty) return path;
    std::string tmp = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-snip-" + snip::file_name(time(nullptr));
    return write_png(flatten(), tmp) ? tmp : "";
  }

  void more_menu(Fl_Widget *w) {
    fld::Menu m;
    m.item("Open in Image Viewer", [this] {
      std::string f = current_file();
      if (!f.empty()) fld::spawn({"fl-view", f});
    });
    m.item("Open With Another Program…", [this] {
      std::string f = current_file();
      if (!f.empty()) fld::spawn({"xdg-open", f});
    });
    m.item("Show in Files", [this] { fld::spawn({"fl-files", fld::dirname(path)}); },
           path.empty() ? FL_MENU_INACTIVE : 0);
    m.item("Print…", [this] {
      std::string f = current_file();
      if (f.empty()) return;
      if (fl_choice("Print the snip on the default printer?", "Cancel", "Print", nullptr) == 1 &&
          !fld::spawn({"lp", f}))
        fl_alert("Printing needs CUPS (lp).");
    }, fld::which("lp").empty() ? FL_MENU_INACTIVE : 0).divider();
    m.submenu("New Snip");
    for (Mode md : {Mode::Rect, Mode::Freeform, Mode::Window, Mode::Full})
      m.item(mode_name(md), [md] { new_snip(md, 0); });
    m.end_submenu();
    m.submenu("New Snip in 5 Seconds");
    for (Mode md : {Mode::Rect, Mode::Freeform, Mode::Window, Mode::Full})
      m.item(mode_name(md), [md] { new_snip(md, 5); });
    m.end_submenu();
    m.item("Settings…", [] { show_settings(); });
    m.item("About", [] {
      fld::about_dialog("Snip", "applets-screenshooter", "Screenshots of a part of the screen, a window or all of it.");
    });
    m.popup(w->x(), w->y() + w->h());
  }

  void context_menu(fld::Menu &m) {
    m.item("Copy", [this] { copy(); }, 0, FL_CTRL + 'c');
    m.item("Save", [this] { save(); }, 0, FL_CTRL + 's');
    m.item("Save As…", [this] { save_as(); }, 0, FL_CTRL + FL_SHIFT + 's').divider();
    m.item("Undo", [this] { undo(); }, undo_stack.empty() ? FL_MENU_INACTIVE : 0, FL_CTRL + 'z');
    m.item("Redo", [this] { redo(); }, redo_stack.empty() ? FL_MENU_INACTIVE : 0, FL_CTRL + FL_SHIFT + 'z');
    m.item("Clear Drawings", [this] {
      push_undo();
      shapes.clear();
      rebuild();
    }, shapes.empty() ? FL_MENU_INACTIVE : 0).divider();
    static const char *names[] = {"Pen", "Highlighter", "Line", "Arrow", "Rectangle", "Ellipse", "Text", "Pixelate",
                                  "Crop", "Eraser"};
    m.submenu("Tool");
    for (int i = 0; i < 10; i++) m.radio(names[i], (int)tool == i, [this, i] { set_tool((Tool)i); });
    m.end_submenu();
    m.submenu("Color");
    for (int i = 0; i < (int)(sizeof kColors / sizeof kColors[0]); i++)
      m.radio(kColorNames[i], color == i, [this, i] {
        color = i;
        color_btn->color(kColors[i]);
        color_btn->redraw();
      });
    m.end_submenu();
    m.divider();
    m.toggle("Fit to Window", canvas->fit, [this] {
      canvas->fit = !canvas->fit;
      if (!canvas->fit) canvas->zoom = 1;
      canvas->redraw();
    });
    m.item("Actual Size", [this] {
      canvas->fit = false;
      canvas->zoom = 1;
      canvas->redraw();
    }, 0, FL_CTRL + '0');
  }

  int handle(int e) override {
    if (e == FL_KEYBOARD || e == FL_SHORTCUT) {
      int k = Fl::event_key(), st = Fl::event_state();
      bool ctrl = st & FL_CTRL, shift = st & FL_SHIFT;
      if (ctrl) {
        switch (k) {
          case 'z': shift ? redo() : undo(); return 1;
          case 'y': redo(); return 1;
          case 'c': copy(); return 1;
          case 's': shift ? save_as() : save(); return 1;
          case 'n': new_snip(S.mode, 0); return 1;
          case 'w': close(); return 1;
          case '0': canvas->fit = false; canvas->zoom = 1; canvas->redraw(); return 1;
          case '=': case '+': canvas->fit = false; canvas->zoom = std::min(8.0, canvas->zoom * 1.25); canvas->redraw(); return 1;
          case '-': canvas->fit = false; canvas->zoom = std::max(0.1, canvas->zoom / 1.25); canvas->redraw(); return 1;
        }
      } else if (e == FL_KEYBOARD && !(st & FL_ALT)) {
        static const char keys[] = "phlaretxcd";
        const char *p = k < 128 ? strchr(keys, k) : nullptr;
        if (p && *p) {
          set_tool((Tool)(p - keys));
          return 1;
        }
        if (k == FL_Escape) return 1; // Esc does not close the editor
      }
    }
    return Fl_Double_Window::handle(e);
  }

  void close() {
    if (dirty && path.empty()) {
      int c = fl_choice("Save the snip before closing?", "Cancel", "Save", "Don't Save");
      if (c == 0) return;
      if (c == 1 && !save()) return;
    } else if (dirty) {
      int c = fl_choice("Save the changes to %s?", "Cancel", "Save", "Don't Save", fld::basename(path).c_str());
      if (c == 0) return;
      if (c == 1 && !save()) return;
    }
    hide();
    Fl::delete_widget(this);
  }
};

double Canvas::ox() const {
  Rect v = ed->view_rect();
  double W = v.w * zoom;
  return x() + std::max(0.0, (w() - W) / 2) - v.x * zoom;
}
double Canvas::oy() const {
  Rect v = ed->view_rect();
  double H = v.h * zoom;
  return y() + std::max(0.0, (h() - H) / 2) - v.y * zoom;
}

Point Canvas::to_image(int ex, int ey) const {
  return {(int)lround((ex - ox()) / zoom), (int)lround((ey - oy()) / zoom)};
}

void Canvas::draw() {
  Rect v = ed->view_rect();
  if (fit) zoom = std::min(1.0, std::min((w() - 16.0) / std::max(1, v.w), (h() - 16.0) / std::max(1, v.h)));
  fl_push_clip(x(), y(), w(), h());
  fl_color(fl_color_average(fld::palette().bg, FL_BLACK, 0.85f));
  fl_rectf(x(), y(), w(), h());
  double X0 = ox(), Y0 = oy();
  int vx = (int)lround(X0 + v.x * zoom), vy = (int)lround(Y0 + v.y * zoom);
  int vw = (int)lround(v.w * zoom), vh = (int)lround(v.h * zoom);
  // a checkerboard under transparent parts
  fl_push_clip(vx, vy, vw, vh);
  for (int yy = vy; yy < vy + vh; yy += 8)
    for (int xx = vx; xx < vx + vw; xx += 8) {
      fl_color(((xx - vx) / 8 + (yy - vy) / 8) % 2 ? fl_rgb_color(0xcc, 0xcc, 0xcc) : FL_WHITE);
      fl_rectf(xx, yy, 8, 8);
    }
  Fl_RGB_Image *img = ed->work_img.get();
  img->scale((int)lround(ed->work.w * zoom), (int)lround(ed->work.h * zoom), 0, 1);
  img->draw((int)lround(X0), (int)lround(Y0));
  for (auto &s : ed->shapes) EditorWindow::draw_shape(s, X0, Y0, zoom);
  if (ed->live) {
    const Shape &s = *ed->live;
    if (s.tool == Tool::Pixelate || s.tool == Tool::Crop || s.tool == Tool::Highlighter) {
      // a preview: the rectangle, or the stroke
      fl_color(s.tool == Tool::Highlighter ? s.color : fld::palette().accent);
      if (s.tool == Tool::Highlighter) {
        fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, std::max(1, (int)lround(s.size * 5 * zoom)));
        fl_begin_line();
        for (auto &p : s.pts) fl_vertex(X0 + p.first * zoom, Y0 + p.second * zoom);
        fl_end_line();
      } else if (s.pts.size() == 2) {
        Rect r = snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second);
        fl_line_style(FL_DASH, 2);
        fl_rect((int)(X0 + r.x * zoom), (int)(Y0 + r.y * zoom), (int)(r.w * zoom), (int)(r.h * zoom));
      }
      fl_line_style(0);
    } else {
      EditorWindow::draw_shape(s, X0, Y0, zoom);
    }
  }
  fl_pop_clip();
  fl_pop_clip();
}

int Canvas::handle(int e) {
  switch (e) {
    case FL_ENTER:
      window()->cursor(ed->tool == Tool::Text ? FL_CURSOR_INSERT : ed->tool == Tool::Eraser ? FL_CURSOR_HAND : FL_CURSOR_CROSS);
      return 1;
    case FL_LEAVE:
      window()->cursor(FL_CURSOR_DEFAULT);
      return 1;
    case FL_MOUSEWHEEL:
      if (Fl::event_state() & FL_CTRL) {
        fit = false;
        zoom = std::max(0.1, std::min(8.0, zoom * (Fl::event_dy() < 0 ? 1.25 : 0.8)));
        redraw();
        return 1;
      }
      return 0;
    case FL_PUSH: {
      if (Fl::event_button() != FL_LEFT_MOUSE) return 0;
      take_focus();
      Point p = to_image(Fl::event_x(), Fl::event_y());
      if (ed->tool == Tool::Eraser) {
        int i = ed->shape_at(p);
        if (i >= 0) {
          ed->push_undo();
          ed->shapes.erase(ed->shapes.begin() + i);
          ed->rebuild();
        }
        return 1;
      }
      if (ed->tool == Tool::Text) {
        const char *t = fl_input("Text:", "");
        if (t && *t) {
          Shape s{Tool::Text, kColors[ed->color], ed->pen_size, {p}, t};
          // the click is the top left of the text: fl_draw takes the baseline
          s.pts[0].second += s.size * 5;
          ed->add_shape(s);
        }
        return 1;
      }
      ed->live_shape = Shape{ed->tool, kColors[ed->color], ed->pen_size, {p}, ""};
      if (ed->tool != Tool::Pen && ed->tool != Tool::Highlighter) ed->live_shape.pts.push_back(p);
      ed->live = &ed->live_shape;
      redraw();
      return 1;
    }
    case FL_DRAG: {
      if (!ed->live) return 1;
      Point p = to_image(Fl::event_x(), Fl::event_y());
      Shape &s = ed->live_shape;
      if (s.tool == Tool::Pen || s.tool == Tool::Highlighter) {
        if (s.pts.back() != p) s.pts.push_back(p);
      } else {
        if (Fl::event_state() & FL_SHIFT && (s.tool == Tool::Line || s.tool == Tool::Arrow)) {
          // Shift: horizontal, vertical or 45 degrees
          int dx = p.first - s.pts[0].first, dy = p.second - s.pts[0].second;
          if (std::abs(dx) > 2 * std::abs(dy)) p.second = s.pts[0].second;
          else if (std::abs(dy) > 2 * std::abs(dx)) p.first = s.pts[0].first;
          else p.second = s.pts[0].second + (dy < 0 ? -1 : 1) * std::abs(dx);
        }
        s.pts[1] = p;
      }
      redraw();
      return 1;
    }
    case FL_RELEASE: {
      if (!ed->live) return 1;
      Shape s = ed->live_shape;
      ed->live = nullptr;
      bool rect_tool = s.tool == Tool::Box || s.tool == Tool::Ellipse || s.tool == Tool::Pixelate || s.tool == Tool::Crop;
      if (rect_tool && s.pts.size() == 2) {
        Rect r = snip::span(s.pts[0].first, s.pts[0].second, s.pts[1].first, s.pts[1].second);
        if (r.w < 3 || r.h < 3) {
          redraw();
          return 1;
        }
      }
      if ((s.tool == Tool::Line || s.tool == Tool::Arrow) && s.pts.size() == 2 && s.pts[0] == s.pts[1]) {
        redraw();
        return 1;
      }
      ed->add_shape(s);
      return 1;
    }
  }
  return Fl_Widget::handle(e);
}

EditorWindow *open_editor(Image img, const std::string &saved_path) {
  auto *e = new EditorWindow(std::move(img));
  e->path = saved_path;
  e->rebuild();
  e->show();
  return e;
}
