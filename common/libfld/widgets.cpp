#include "fld/widgets.h"
#include "fld/icons.h"
#include "fld/theme.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

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
