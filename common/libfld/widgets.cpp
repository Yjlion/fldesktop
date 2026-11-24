#include "fld/widgets.h"
#include "fld/icons.h"
#include "fld/text.h"
#include "fld/theme.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>

namespace fld {

FlatButton::FlatButton(int x, int y, int w, int h, const char *l) : Fl_Button(x, y, w, h, l) {
  box(FL_FLAT_BOX);
  down_box(FL_FLAT_BOX);
  clear_visible_focus();
}

void FlatButton::icon_name(const char *name, int size) {
  icon_ = icon(name, size);
  redraw();
}

Fl_Color FlatButton::hover_c() const {
  if (hover_col_) return hover_col_;
  return fl_color_average(color(), palette().dark ? FL_WHITE : FL_BLACK, 0.88f);
}

int FlatButton::handle(int e) {
  switch (e) {
    case FL_ENTER: hover_ = true; redraw(); return 1;
    case FL_LEAVE: hover_ = false; redraw(); break;
    default: break;
  }
  return Fl_Button::handle(e);
}

void FlatButton::draw() {
  bool pressed = value() || active_look_;
  fl_color(color());
  fl_rectf(x(), y(), w(), h());
  if (pressed || (hover_ && active_r())) {
    fl_color(pressed ? fl_color_average(color(), palette().accent, 0.65f) : hover_c());
    fl_rounded_rectf(x() + 1, y() + 1, w() - 2, h() - 2, 5);
  }
  int ix = x(), iw = 0;
  const char *l = label();
  fl_font(labelfont(), labelsize());
  int lw = 0, lh = 0;
  if (l && *l) fl_measure(l, lw, lh, 0);
  if (icon_) iw = icon_->w();
  int total = iw + (iw && lw ? 6 : 0) + lw;
  ix = (align() & FL_ALIGN_LEFT) ? x() + 6 : x() + (w() - total) / 2;
  if (icon_) {
    if (active_r()) icon_->draw(ix, y() + (h() - icon_->h()) / 2);
    else {
      icon_->draw(ix, y() + (h() - icon_->h()) / 2);
      fl_color(fl_color_average(color(), FL_BLACK, 0.5f));
    }
    ix += iw + (lw ? 6 : 0);
  }
  if (lw) {
    fl_color(active_r() ? labelcolor() : fl_inactive(labelcolor()));
    fl_draw(escape_at(l).c_str(), ix, y(), w() - (ix - x()), h(), FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  }
  if (Fl::focus() == this && visible_focus()) draw_focus();
}

void draw_focus_ring(int x, int y, int w, int h, int radius) {
  fl_color(palette().accent);
  fl_line_style(FL_SOLID, 2);
  fl_rounded_rect(x + 1, y + 1, w - 2, h - 2, radius);
  fl_line_style(0);
}

// ---- tabs ----

Tabs::Tabs(int x, int y, int w, int h, const char *l) : Fl_Tabs(x, y, w, h, l) {
  box(FL_FLAT_BOX);
  color(palette().bg);
  selection_color(palette().bg);
}

void Tabs::draw() {
  // the strip behind the tabs, and a line under them
  if (damage() & FL_DAMAGE_ALL) {
    int H = tab_height();
    int ty = H >= 0 ? y() : y() + h() + H, th = H >= 0 ? H : -H;
    fl_color(palette().bg);
    fl_rectf(x(), ty, w(), th);
  }
  Fl_Tabs::draw();
  int H = tab_height();
  if (H > 0) {
    fl_color(palette().border);
    fl_xyline(x(), y() + H - 1, x() + w() - 1);
  }
}

void Tabs::draw_tab(int x1, int x2, int W, int H, Fl_Widget *o, int flags, int what) {
  const Palette &p = palette();
  // Fl_Tabs.cxx's own (file-local) enum { LEFT, RIGHT, SELECTED }
  const int kRight = 1, kSelected = 2;
  bool sel = what == kSelected;
  if (x2 < x1 + W && what == kRight) x1 = x2 - W;
  bool top = H >= 0;
  if (!top) H = -H;
  int ty = top ? y() : y() + h() - H;
  fl_push_clip(x1, ty, W, H);
  fl_color(p.bg);
  fl_rectf(x1, ty, W, H);
  Fl_Color lc = o->labelcolor();
  Fl_Font lf = o->labelfont();
  o->labelcolor(sel ? p.fg : p.fg_dim);
  o->labelfont(sel ? (lf | FL_BOLD) : lf);
  o->draw_label(x1, ty, W, H - (sel ? 3 : 0), FL_ALIGN_CENTER);
  o->labelcolor(lc);
  o->labelfont(lf);
  if (sel) {
    // the accent under the shown tab (over it when the tabs are at the bottom)
    fl_color(o->active_r() ? p.accent : p.fg_dim);
    fl_rectf(x1 + 6, top ? ty + H - 3 : ty, W - 12, 3);
  }
  if (Fl::focus() == this && visible_focus() && sel) draw_focus_ring(x1 + 1, ty + 1, W - 2, H - 4);
  fl_pop_clip();
}

// ---- check boxes and radio buttons ----

namespace {

// the box or circle at the left of the button, and where its label goes
void indicator_geometry(Fl_Widget *b, int &bx, int &by, int &s) {
  s = std::min(16, b->h() - 4);
  bx = b->x() + Fl::box_dx(b->box()) + 3;
  by = b->y() + (b->h() - s) / 2;
}

} // namespace

void CheckButton::draw() {
  const Palette &p = palette();
  if (box()) draw_box(box(), color());
  int bx, by, s;
  indicator_geometry(this, bx, by, s);
  bool on = value() != 0;
  Fl_Color fill = on ? (active_r() ? p.accent : p.fg_dim) : p.bg2;
  fl_color(fill);
  fl_rounded_rectf(bx, by, s, s, 3);
  if (!on) {
    fl_color(active_r() ? fl_color_average(p.fg, p.bg2, 0.45f) : p.border);
    fl_rounded_rect(bx, by, s, s, 3);
  } else {
    fl_color(text_on(fill));
    fl_line_style(FL_SOLID | FL_CAP_ROUND | FL_JOIN_ROUND, 2);
    fl_begin_line();
    fl_vertex(bx + s * 0.24, by + s * 0.52);
    fl_vertex(bx + s * 0.42, by + s * 0.70);
    fl_vertex(bx + s * 0.76, by + s * 0.32);
    fl_end_line();
    fl_line_style(0);
  }
  // 6 px between the box and its label
  int lx = bx + s + 6;
  draw_label(lx, y(), x() + w() - lx, h(), (align() & ~(FL_ALIGN_RIGHT | FL_ALIGN_CENTER)) | FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  if (Fl::focus() == this && visible_focus()) draw_focus_ring(bx - 3, by - 3, s + 6, s + 6, 5);
}

void RadioButton::draw() {
  const Palette &p = palette();
  if (box()) draw_box(box(), color());
  int bx, by, s;
  indicator_geometry(this, bx, by, s);
  bool on = value() != 0;
  fl_color(on ? (active_r() ? p.accent : p.fg_dim) : p.bg2);
  fl_pie(bx, by, s, s, 0, 360);
  if (!on) {
    fl_color(active_r() ? fl_color_average(p.fg, p.bg2, 0.45f) : p.border);
    fl_arc(bx, by, s, s, 0, 360);
  } else {
    int d = s * 2 / 5;
    fl_color(text_on(active_r() ? p.accent : p.fg_dim));
    fl_pie(bx + (s - d) / 2, by + (s - d) / 2, d, d, 0, 360);
  }
  int lx = bx + s + 6;
  draw_label(lx, y(), x() + w() - lx, h(), (align() & ~(FL_ALIGN_RIGHT | FL_ALIGN_CENTER)) | FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  if (Fl::focus() == this && visible_focus()) draw_focus_ring(bx - 3, by - 3, s + 6, s + 6, s);
}

// ---- search fields ----

namespace {

// the box of a search field: a thin frame with room for the magnifier on the
// left (as a box type, so Fl_Input puts the text and the clicks inside it)
void search_box(int x, int y, int w, int h, Fl_Color c) {
  const Palette &p = palette();
  fl_color(c);
  fl_rectf(x, y, w, h);
  fl_color(p.border);
  fl_rect(x, y, w, h);
  if (Fl_Image *im = symbolic_icon("system-search", 16, p.fg_dim)) im->draw(x + 6, y + (h - 16) / 2);
}

Fl_Boxtype search_boxtype() {
  static Fl_Boxtype t = FL_NO_BOX;
  if (t == FL_NO_BOX) {
    t = (Fl_Boxtype)(FL_FREE_BOXTYPE + 7);
    Fl::set_boxtype(t, search_box, 27, 2, 30, 4);
  }
  return t;
}

} // namespace

SearchInput::SearchInput(int x, int y, int w, int h, const char *ph) : Fl_Input(x, y, w, h) {
  box(search_boxtype());
  color(palette().bg2);
  if (ph) placeholder_ = ph;
}

void SearchInput::draw() {
  const Palette &p = palette();
  Fl_Input::draw();
  if (!(damage() & FL_DAMAGE_ALL) && !(damage() & FL_DAMAGE_EXPOSE)) return;
  // the accent around the field while typing in it
  if (Fl::focus() == this) {
    fl_color(p.accent);
    fl_rect(x(), y(), w(), h());
    fl_rect(x() + 1, y() + 1, w() - 2, h() - 2);
  }
  if (!size() && !placeholder_.empty() && Fl::focus() != this) {
    fl_font(textfont(), textsize());
    fl_color(p.fg_dim);
    fl_draw(placeholder_.c_str(), x() + Fl::box_dx(box()) + 2, y(), w() - Fl::box_dw(box()) - 4, h(),
            FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  }
}

Toolbar::Toolbar(int x, int y, int w, int h) : Fl_Group(x, y, w, h), next_x_(x + 4) {
  box(FL_FLAT_BOX);
  end();
  resizable(nullptr);
}

FlatButton *Toolbar::add_button(const char *icon, const char *tooltip, Fl_Callback *cb, void *data,
                                const char *label) {
  int bh = h() - 6;
  int bw = bh;
  if (label) {
    fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
    bw = (int)fl_width(label) + (icon ? 34 : 16);
  }
  FlatButton *b = new FlatButton(next_x_, y() + 3, bw, bh, label);
  b->color(color());
  if (icon) b->icon_name(icon, 20);
  if (tooltip) b->copy_tooltip(tooltip);
  b->callback(cb, data);
  add(b);
  next_x_ += bw + 2;
  return b;
}

void Toolbar::add_separator() {
  Fl_Box *s = new Fl_Box(next_x_ + 3, y() + 6, 1, h() - 12);
  s->box(FL_FLAT_BOX);
  s->color(palette().border);
  add(s);
  next_x_ += 8;
}

void Toolbar::add_widget(Fl_Widget *w) {
  w->position(next_x_, y() + (h() - w->h()) / 2);
  add(w);
  next_x_ += w->w() + 4;
}

void Toolbar::draw() {
  Fl_Group::draw();
  fl_color(palette().border);
  fl_xyline(x(), y() + h() - 1, x() + w() - 1);
}

std::string escape_at(const std::string &s) {
  std::string r;
  for (char c : s) {
    if (c == '@') r += '@';
    r += c;
  }
  return r;
}

void about_dialog(const char *app_name, const char *icon_n, const char *description) {
  Fl_Window *w = new Fl_Window(360, 260, "About");
  w->set_modal();
  Fl_Box *ic = new Fl_Box(0, 16, 360, 72);
  if (Fl_Image *i = icon(icon_n, 64)) ic->image(i);
  Fl_Box *name = new Fl_Box(0, 92, 360, 30, app_name);
  name->labelfont(FL_HELVETICA_BOLD);
  name->labelsize(20);
  static char ver[64];
  snprintf(ver, sizeof ver, "fldesktop %s", FLD_VERSION);
  Fl_Box *v = new Fl_Box(0, 122, 360, 20, ver);
  v->labelcolor(palette().fg_dim);
  Fl_Box *d = new Fl_Box(20, 146, 320, 60, description);
  d->align(FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_TOP);
  Fl_Button *ok = new Fl_Button(140, 216, 80, 30, "Close");
  ok->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  w->end();
  w->show();
  while (w->shown()) Fl::wait();
  Fl::delete_widget(w);
}

} // namespace fld
