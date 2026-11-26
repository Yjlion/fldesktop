// fl-users: Users and Login Screen of fl-settings as an administration tool
// of its own, for systems where fl-settings is not used. The pages are the
// same sources (pages/users.cpp, pages/login.cpp); changes go through
// pkexec fl-settings-helper like in Settings.
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Tabs.H>
#include <algorithm>
#include <cstdio>
#include <string>

#include "fld/i18n.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fld/window_size.h"
#include "settings_page.h"

namespace fld {
PageInfo page_users();
PageInfo page_login();

const std::vector<PageInfo> &all_pages() {
  static std::vector<PageInfo> pages = {page_users(), page_login()};
  return pages;
}
const std::vector<std::pair<std::string, std::string>> &skipped_pages() {
  static std::vector<std::pair<std::string, std::string>> none;
  return none;
}
}  // namespace fld

static Fl_Tabs *g_tabs;
static const int W = 860, H = 620;

// the lowest point of the widgets in g
static int content_bottom(Fl_Group *g) {
  int b = g->y();
  for (int i = 0; i < g->children(); i++) {
    Fl_Widget *c = g->child(i);
    if (!c->visible()) continue;
    b = std::max(b, c->y() + c->h());
    if (Fl_Group *cg = c->as_group()) b = std::max(b, content_bottom(cg));
  }
  return b;
}

// a page in a scrolled tab: the pages are taller than the window
static Fl_Group *make_tab(const fld::PageInfo &p) {
  g_tabs->begin();
  Fl_Scroll *s = new Fl_Scroll(0, 30, W, H - 30);
  s->type(Fl_Scroll::VERTICAL);
  s->color(fld::palette().bg2);
  int pw = W - Fl::scrollbar_size();
  Fl_Group *g = p.create(0, 30, pw, H - 30);
  int b = content_bottom(g);
  if (b + 16 > g->y() + g->h()) g->size(pw, b + 16 - g->y());
  s->end();
  s->copy_label(p.title.c_str());
  g_tabs->end();
  return s;
}

namespace fld {
// a page asks to be built again (after a reset): the tab is replaced
void reload_current_page() {
  Fl_Widget *cur = g_tabs->value();
  int i = g_tabs->find(cur);
  if (i >= g_tabs->children()) return;
  g_tabs->remove(cur);
  Fl::delete_widget(cur);
  Fl_Group *g = make_tab(all_pages()[(size_t)i]);
  g_tabs->insert(*g, i);
  g_tabs->value(g);
  g_tabs->redraw();
}
}  // namespace fld

int main(int argc, char **argv) {
  fld::i18n_init();
  std::string start;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("%s", _("Usage: fl-users [users|login]\n"
                     "Adds, removes and changes user accounts and sets up the login screen.\n"));
      return 0;
    }
    if (a[0] != '-') start = a;
  }
  fld::init_app("fl-users", argc, argv);
  Fl::lock();
  Fl_Double_Window *win = new Fl_Double_Window(W, H, _("Users"));
  g_tabs = new fld::Tabs(0, 0, W, H);
  g_tabs->end();
  for (auto &p : fld::all_pages()) {
    Fl_Group *g = make_tab(p);
    if (p.id == start) g_tabs->value(g);
  }
  win->resizable(g_tabs);
  win->end();
  win->size_range(640, 480);
  fld::set_window_icon(win, "fl-users");
  fld::remember_window_size(win, "fl-users");
  win->show();  // not show(argc, argv): it resets the theme colors
  return Fl::run();
}
