#include "settings_common.h"

#include <FL/Fl.H>
#include <unistd.h>

#include "fld/theme.h"
#include "fld/util.h"

namespace fld {

std::string settings_helper_path() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) {
    buf[n] = 0;
    std::string local = dirname(buf) + "/fl-settings-helper";
    if (file_exists(local) && !starts_with(local, "/usr/")) return local;
  }
  return std::string(FLD_LIBEXECDIR) + "/fl-settings-helper";
}

Form::Form(Fl_Group *g, int x, int y, int w, int label_w) : g_(g), x_(x), y_(y), w_(w), label_w_(label_w) {}

Fl_Box *Form::heading(const char *text) {
  if (y_ > g_->y() + 70) y_ += 10;
  Fl_Box *b = new Fl_Box(x_, y_, w_, 26, text);
  b->labelfont(FL_HELVETICA_BOLD);
  b->labelsize(14);
  b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  y_ += 30;
  return b;
}

Fl_Box *Form::note(const char *text, int lines) {
  Fl_Box *b = new Fl_Box(x_ + label_w_, y_, w_ - label_w_, 18 * lines + 2, text);
  b->labelsize(12);
  b->labelcolor(palette().fg_dim);
  b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  y_ += 18 * lines + 8;
  return b;
}

void Form::row(const char *label, int h, int &wx, int &wy, int &ww) {
  if (label && *label) {
    // copied: callers pass temporary strings (the About page's rows)
    Fl_Box *l = new Fl_Box(x_, y_, label_w_ - 10, h);
    l->copy_label(label);
    l->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  }
  wx = x_ + label_w_;
  wy = y_;
  ww = w_ - label_w_;
  y_ += h + 8;
}

std::string wm_ini_path() { return fld_config_dir() + "/wm.ini"; }
std::string desktop_ini_path() { return fld_config_dir() + "/desktop.ini"; }
std::string panel_ini_path() { return fld_config_dir() + "/panel.ini"; }

void save_wm(ConfigFile &f) {
  f.save();
  reload_wm();
}

Fl_Group *page_group(int x, int y, int w, int h, const char *title, int &content_y) {
  Fl_Group *g = new Fl_Group(x, y, w, h);
  g->box(FL_FLAT_BOX);
  g->color(palette().bg2);
  Fl_Box *t = new Fl_Box(x + 24, y + 14, w - 48, 34, title);
  t->labelfont(FL_HELVETICA_BOLD);
  t->labelsize(20);
  t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  content_y = y + 60;
  g->resizable(nullptr); // rows keep their size when the window grows
  return g;
}

} // namespace fld
