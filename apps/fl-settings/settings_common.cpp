#include "settings_common.h"

#include <FL/Fl.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <unistd.h>

#include "fld/i18n.h"
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

void reset_keys(ConfigFile &f, const ConfigKeys &keys) {
  f.ini.load(f.path);
  for (auto &k : keys) f.ini.remove(k.first, k.second);
  if (f.path == wm_ini_path()) save_wm(f);
  else f.save();
}

namespace {
struct ResetData {
  ConfigFile *file;
  ConfigKeys keys;
  std::function<void()> done;
};
} // namespace

Fl_Button *reset_page_button(int x, int y, std::vector<std::pair<std::string, ConfigKeys>> files) {
  // the ConfigFiles live as long as the button (the page)
  struct Files { std::vector<std::pair<ConfigFile, ConfigKeys>> list; };
  Files *fs = new Files;
  for (auto &f : files) fs->list.emplace_back(ConfigFile(f.first), f.second);
  ConfigFile *first = &fs->list.front().first;
  ConfigKeys first_keys = fs->list.front().second;
  return reset_button(x, y, *first, first_keys, [fs] {
    for (size_t i = 1; i < fs->list.size(); i++) reset_keys(fs->list[i].first, fs->list[i].second);
    // later, outside the button's callback: the page (and the button) are deleted
    Fl::add_timeout(0, [](void *) { reload_current_page(); });
  });
}

Fl_Button *reset_button(int x, int y, ConfigFile &f, ConfigKeys keys, std::function<void()> done) {
  const char *l = _("Reset to Defaults");
  fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
  Fl_Button *b = new Fl_Button(x, y, std::max(120, (int)fl_width(l) + 24), 30, l);
  b->tooltip(_("Use the default values for the settings on this page"));
  ResetData *d = new ResetData{&f, std::move(keys), std::move(done)};
  b->callback([](Fl_Widget *, void *p) {
    ResetData *d = (ResetData *)p;
    if (fl_choice("%s", _("Cancel"), _("Reset"), nullptr, _("Use the default values for the settings on this page?")) != 1) return;
    reset_keys(*d->file, d->keys);
    if (d->done) d->done();
  }, d);
  return b;
}

Fl_Group *page_group(int x, int y, int w, int h, const char *title, int &content_y) {
  Fl_Group *g = new Fl_Group(x, y, w, h);
  g->box(FL_FLAT_BOX);
  g->color(palette().bg2);
  Fl_Box *t = new Fl_Box(x + 24, y + 14, w - 48, 34, _(title));
  t->labelfont(FL_HELVETICA_BOLD);
  t->labelsize(20);
  t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  content_y = y + 60;
  g->resizable(nullptr); // rows keep their size when the window grows
  return g;
}

} // namespace fld
