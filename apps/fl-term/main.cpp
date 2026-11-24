#include "fld/json.h" // first: X11 headers define Bool
// fl-term: terminal emulator for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Hor_Value_Slider.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_ask.H>
#include <FL/platform.H>
#if defined(FLTK_USE_WAYLAND)
#include <wayland-client.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "ssh_ui.h"
#include "fld/menus.h"
#include "fld/tab_bar.h"
#include "fld/tab_move.h"
#include "term_panes.h"
#include "term_settings.h"
#include "term_widget.h"

static const char *kUsage =
    "Usage: fl-term [options] [-e command [args...]]\n"
    "  -e, --command CMD...    run CMD instead of the shell (rest of the arguments)\n"
    "  -d, --workdir DIR       start in DIR\n"
    "  -T, --title TITLE       window title\n"
    "  -g, --geometry COLSxROWS  initial size (default 90x26)\n"
    "      --hold              keep the window open after the command exits\n"
    "      --ssh PROFILE       connect to a saved SSH connection, an ~/.ssh/config host\n"
    "                          or user@host[:port]\n"
    "      --scheme NAME       color scheme: dark, light, red, green, blue\n"
    "      --profile NAME      start with a saved profile (Profiles in the menu)\n"
    "  -v, --version           show the version\n"
    "Tabs: Ctrl+Shift+T new tab, Ctrl+Shift+W close tab (or pane), Ctrl+PgUp/PgDn switch tabs.\n"
    "Panes: Ctrl+Shift+E side by side, Ctrl+Shift+O above and below, Ctrl+Shift+arrows move.\n"
    "Search: Ctrl+Shift+F.\n"
    "  -h, --help              show this help\n";

struct Scheme {
  const char *name;
  TermColors c;
};

// fg, bg, cursor, selection, then the 16 ANSI colors
static const Scheme kSchemes[] = {
    {"dark",
     {{0xdc, 0xdf, 0xe4}, {0x1e, 0x1f, 0x22}, {0xe6, 0xe6, 0xe6}, {0x3a, 0x4d, 0x6b},
      {{0x2e, 0x34, 0x36}, {0xe0, 0x4b, 0x4b}, {0x6a, 0xc1, 0x4b}, {0xe5, 0xc0, 0x4b},
       {0x4b, 0x8c, 0xe0}, {0xb0, 0x6b, 0xd8}, {0x3f, 0xbf, 0xbf}, {0xd3, 0xd7, 0xcf},
       {0x6e, 0x75, 0x7a}, {0xff, 0x6e, 0x6e}, {0x8a, 0xe2, 0x34}, {0xfc, 0xe9, 0x4f},
       {0x72, 0x9f, 0xcf}, {0xd4, 0x8f, 0xe6}, {0x63, 0xe0, 0xe0}, {0xff, 0xff, 0xff}}}},
    {"light",
     {{0x24, 0x26, 0x2b}, {0xfa, 0xfa, 0xfa}, {0x24, 0x26, 0x2b}, {0xb4, 0xd2, 0xf5},
      {{0x24, 0x26, 0x2b}, {0xc0, 0x1c, 0x28}, {0x26, 0xa2, 0x69}, {0xa2, 0x73, 0x04},
       {0x1c, 0x71, 0xd8}, {0x81, 0x3d, 0x9c}, {0x22, 0x89, 0x8a}, {0xb0, 0xb3, 0xb8},
       {0x5e, 0x5c, 0x64}, {0xe0, 0x1b, 0x24}, {0x2e, 0xc2, 0x7e}, {0xe5, 0xa5, 0x0a},
       {0x35, 0x84, 0xe4}, {0x91, 0x41, 0xac}, {0x33, 0xc7, 0xde}, {0x24, 0x26, 0x2b}}}},
};

// dark with a tinted background, for telling remote sessions apart
static TermColors tinted(uint8_t r, uint8_t g, uint8_t b) {
  TermColors c = kSchemes[0].c;
  c.bg[0] = r; c.bg[1] = g; c.bg[2] = b;
  c.palette[0][0] = r; c.palette[0][1] = g; c.palette[0][2] = b;
  return c;
}

static const TermColors *find_scheme(const std::string &name) {
  static TermColors red = tinted(0x33, 0x16, 0x18), green = tinted(0x14, 0x2b, 0x1c), blue = tinted(0x14, 0x22, 0x38);
  if (name == "red") return &red;
  if (name == "green") return &green;
  if (name == "blue") return &blue;
  for (auto &s : kSchemes)
    if (name == s.name) return &s.c;
  return nullptr;
}

// ---- windows, tabs and settings ------------------------------------------------

struct TabSpec {
  std::vector<std::string> cmd;
  std::string cwd, title, scheme, profile;
  bool hold = false, hold_on_error = false;
};

struct Tab {
  TermWidget *term;   // the pane with the focus
  bool own_scheme; // keeps its colour scheme (SSH profiles, --scheme)
  TabSpec spec;       // how it was started (Duplicate Tab)
  std::string custom; // a name given with Rename Tab (wins over the title)
  std::vector<TermWidget *> panes{}; // all of the tab's terminals
  term::PaneTree tree{0};            // how they share the tab
};
static const int PANE_GAP = 4;

struct App {
  Fl_Double_Window *win = nullptr;
  Fl_Menu_Bar *menubar = nullptr;
  fld::TabBar *tabbar = nullptr;
  Fl_Group *area = nullptr; // holds the terminals; only the current one is visible
  std::vector<Tab> tabs;
  int current = 0;
  std::string fixed_title;
  term::Settings set;
  bool hold = false, hold_on_error = false;
  std::vector<term::Profile> profiles;
  // the search bar (Ctrl+Shift+F)
  Fl_Group *search = nullptr;
  Fl_Input *search_in = nullptr;
  Fl_Check_Button *search_case = nullptr;
  Fl_Box *search_msg = nullptr;
};

static App g;

// FLTK declares its rectangular windows opaque to the compositor; a
// translucent background needs that undone (and redone at full opacity, which
// lets the compositor skip what is behind the window).
static void update_opaque_region() {
#if defined(FLTK_USE_WAYLAND)
  if (!g.win || !g.win->shown() || !fl_wl_display()) return;
  struct wl_surface *surf = fl_wl_surface(fl_wl_xid(g.win));
  if (!surf) return;
  if (g.set.opacity < 0.999) {
    wl_surface_set_opaque_region(surf, nullptr);
  } else {
    struct wl_region *r = wl_compositor_create_region(fl_wl_compositor());
    wl_region_add(r, 0, 0, 1000000, 1000000);
    wl_surface_set_opaque_region(surf, r);
    wl_region_destroy(r);
  }
  g.win->redraw(); // the region applies with the next commit
#endif
}

static TermWidget *cur() {
  if (g.tabs.empty()) return nullptr;
  return g.tabs[(size_t)std::max(0, std::min(g.current, (int)g.tabs.size() - 1))].term;
}

static int index_of(TermWidget *t) {
  for (size_t i = 0; i < g.tabs.size(); i++)
    for (TermWidget *p : g.tabs[i].panes)
      if (p == t) return (int)i;
  return -1;
}

static term::Rect area_rect() { return {g.area->x(), g.area->y(), g.area->w(), g.area->h()}; }

static void layout_tab(Tab &tb) {
  for (auto &pr : tb.tree.layout(area_rect(), PANE_GAP)) {
    TermWidget *t = (TermWidget *)pr.first;
    t->resize(pr.second.x, pr.second.y, std::max(1, pr.second.w), std::max(1, pr.second.h));
  }
}

static void load_profiles() {
  fld::Ini ini;
  ini.load(term::config_path());
  g.profiles = term::load_profiles(ini);
}

static const TermColors *scheme_colors(const std::string &name) {
  const TermColors *c = find_scheme(name);
  return c ? c : &kSchemes[0].c;
}

static void update_title() {
  TermWidget *t = cur();
  if (!g.fixed_title.empty()) {
    g.win->copy_label(g.fixed_title.c_str());
  } else {
    std::string tt = t ? (!g.tabs[(size_t)g.current].custom.empty() ? g.tabs[(size_t)g.current].custom : t->title()) : "";
    g.win->copy_label(tt.empty() ? "Terminal" : tt.c_str());
  }
  if (g.tabbar) {
    std::vector<fld::TabInfo> tabs;
    for (auto &tb : g.tabs) {
      fld::TabInfo ti;
      ti.label = !tb.custom.empty() ? tb.custom : tb.term->title();
      std::string dir = tb.term->current_dir();
      ti.tooltip = dir.empty() ? ti.label : dir;
      tabs.push_back(ti);
    }
    g.tabbar->set_tabs(tabs, g.current);
  }
}

// Places the menu bar, tab bar and terminals for the current settings. The
// window keeps its size; the terminal area shrinks or grows.
static void relayout() {
  int W = g.win->w(), H = g.win->h();
  int y = 0;
  if (g.set.show_menubar) {
    int mh = std::max(24, fld::ui_font_size() + 12);
    g.menubar->resize(0, 0, W, mh);
    g.menubar->show();
    y += mh;
  } else {
    g.menubar->hide();
  }
  if (term::tabs_visible(g.set.tabs, (int)g.tabs.size())) {
    int th = fld::TabBar::preferred_height();
    g.tabbar->resize(0, y, W, th);
    g.tabbar->show();
    y += th;
  } else {
    g.tabbar->hide();
  }
  int sh = g.search && g.search->visible() ? 34 : 0;
  g.area->resize(0, y, W, std::max(1, H - y - sh));
  if (sh) {
    g.search->resize(0, H - sh, W, sh);
    int bx = W - 8;
    // the right side: close, case, next, previous; the input takes the rest
    Fl_Widget *close = g.search->child(4), *cs = g.search->child(2), *nx = g.search->child(5), *pv = g.search->child(1);
    close->resize(bx - 28, H - sh + 4, 28, 26);
    bx -= 34;
    cs->resize(bx - 110, H - sh + 4, 110, 26);
    bx -= 116;
    nx->resize(bx - 34, H - sh + 4, 34, 26);
    pv->resize(bx - 70, H - sh + 4, 34, 26);
    bx -= 76;
    g.search_msg->resize(bx - 120, H - sh + 4, 120, 26);
    bx -= 124;
    g.search_in->resize(8, H - sh + 4, std::max(60, bx - 8), 26);
  }
  for (auto &tb : g.tabs) layout_tab(tb);
  // Only a minimum size: cell-sized resize steps made maximized and full
  // screen windows round down and leave a gap. Sent again only when it changes.
  if (TermWidget *t = cur()) {
    static int last_w = -1, last_h = -1;
    int mw = t->cell_w() * 10, mh = t->cell_h() * 3 + y;
    if (mw != last_w || mh != last_h) {
      g.win->size_range(mw, mh);
      last_w = mw;
      last_h = mh;
    }
  }
  g.win->redraw();
}

class MainWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  void resize(int x, int y, int w, int h) override {
    // Fl_Double_Window::resize also resizes the Wayland surface's buffer
    // (Fl_Widget::resize did not: a maximized window kept its old size in a
    // corner); relayout() then places the children.
    Fl_Double_Window::resize(x, y, w, h);
    if (g.area) relayout();
  }
  // On Wayland FLTK already draws into a back buffer that it copies as is to
  // the compositor; Fl_Double_Window's extra offscreen would be blended onto
  // it and lose the translucent background.
  void flush() override {
#if defined(FLTK_USE_WAYLAND)
    if (fl_wl_display()) {
      Fl_Window::flush();
      return;
    }
#endif
    Fl_Double_Window::flush();
  }
};

static void select_tab(int i) {
  if (g.tabs.empty()) return;
  i = (i % (int)g.tabs.size() + (int)g.tabs.size()) % (int)g.tabs.size();
  g.current = i;
  for (size_t k = 0; k < g.tabs.size(); k++)
    for (TermWidget *p : g.tabs[k].panes) {
      if ((int)k == i) p->show();
      else p->hide();
    }
  update_title();
  cur()->take_focus();
  g.win->redraw();
}

static void close_tab(int i) {
  if (i < 0 || i >= (int)g.tabs.size()) return;
  if (g.tabs.size() == 1) {
    g.win->hide();
    return;
  }
  std::vector<TermWidget *> panes = g.tabs[(size_t)i].panes;
  g.tabs.erase(g.tabs.begin() + i);
  for (TermWidget *t : panes) {
    g.area->remove(t);
    Fl::delete_widget(t); // the destructor ends the child process
  }
  if (g.current >= (int)g.tabs.size() || g.current > i) g.current = std::max(0, g.current - 1);
  if (g.current == i && i < (int)g.tabs.size()) g.current = i;
  relayout();
  select_tab(g.current);
}

// moves tab from to index to (dragged, or Move Tab Left/Right)
static void reorder_tab(int from, int to) {
  int n = (int)g.tabs.size();
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
  fld::move_element(g.tabs, from, to);
  g.current = fld::moved_index(g.current, from, to);
  update_title();
}

static void move_tab(int delta) { reorder_tab(g.current, g.current + delta); }

static void on_title(TermWidget *t, const char *, void *) {
  (void)t;
  update_title();
}

static void close_pane(TermWidget *t);

static void on_exit(TermWidget *t, int, void *) {
  // closing from inside the terminal's own callback: defer
  Fl::add_timeout(0, [](void *v) { close_pane((TermWidget *)v); }, t);
}

// a click (or Ctrl+Shift+arrow) in a pane makes it the tab's focused one
static void on_focus(TermWidget *t, void *) {
  int i = index_of(t);
  if (i < 0) return;
  if (g.tabs[(size_t)i].term != t) {
    g.tabs[(size_t)i].term = t;
    update_title();
    g.area->redraw();
  }
}

static void show_menu(TermWidget *, void *);

// font, colours and callbacks for a new terminal
static void setup_term(TermWidget *t, const TabSpec &spec) {
  const term::Profile *pr = term::find_profile(g.profiles, spec.profile);
  int fs = pr && pr->font_size > 0 ? pr->font_size : g.set.font_size;
  if (fs > 0) t->set_font_size(fs);
  std::string scheme = !spec.scheme.empty() ? spec.scheme : pr && !pr->scheme.empty() ? pr->scheme : g.set.scheme;
  t->set_colors(*scheme_colors(scheme));
  t->set_opacity(pr && pr->opacity > 0 ? pr->opacity : g.set.opacity);
  t->set_scrollback_limit((size_t)g.set.scrollback);
  t->set_hold(spec.hold);
  t->set_hold_on_error(spec.hold_on_error);
  t->on_title = on_title;
  t->on_exit = on_exit;
  t->on_menu = show_menu;
  t->on_focus = on_focus;
  if (!spec.title.empty()) t->set_title(spec.title);
}

// a profile's command and folder go into a tab's spec
static void apply_profile(TabSpec &s, const std::string &name) {
  const term::Profile *p = term::find_profile(g.profiles, name);
  if (!p) return;
  s.profile = name;
  if (s.cmd.empty() && !p->command.empty()) {
    bool shell = p->command.find_first_of("|&;<>()$`*?~\"'") != std::string::npos;
    s.cmd = shell ? std::vector<std::string>{"/bin/sh", "-c", p->command} : fld::shell_split(p->command);
  }
  if (!p->cwd.empty()) s.cwd = fld::expand_tilde(p->cwd);
}


static TermWidget *add_tab(const TabSpec &spec) {
  g.area->begin();
  TermWidget *t = new TermWidget(g.area->x(), g.area->y(), g.area->w(), g.area->h());
  g.area->end();
  setup_term(t, spec);
  g.tabs.push_back({t, !spec.scheme.empty() || !spec.profile.empty(), spec, ""});
  g.tabs.back().panes = {t};
  g.tabs.back().tree.reset((intptr_t)t);
  g.current = (int)g.tabs.size() - 1;
  relayout();
  if (!t->start(spec.cmd, spec.cwd)) {
    fl_alert("Could not start the terminal process.");
    close_tab(index_of(t));
    return nullptr;
  }
  select_tab(g.current);
  return t;
}

static void new_tab(const std::string &profile = "") {
  TabSpec s;
  if (TermWidget *t = cur()) s.cwd = t->current_dir();
  apply_profile(s, profile.empty() ? g.set.default_profile : profile);
  add_tab(s);
}

// ---- split panes ----------------------------------------------------------------

// Ctrl+Shift+E (side by side) and Ctrl+Shift+O (above and below): a shell in
// the focused pane's folder, sharing its place
static void split_pane(bool side_by_side) {
  if (g.tabs.empty()) return;
  Tab &tb = g.tabs[(size_t)g.current];
  TermWidget *from = tb.term;
  TabSpec s;
  s.cwd = from->current_dir();
  s.profile = tb.spec.profile;
  if (!s.profile.empty()) apply_profile(s, s.profile);
  g.area->begin();
  TermWidget *t = new TermWidget(g.area->x(), g.area->y(), 100, 100);
  g.area->end();
  setup_term(t, s);
  if (tb.own_scheme && !tb.spec.scheme.empty()) t->set_colors(*scheme_colors(tb.spec.scheme));
  tb.tree.split((intptr_t)from, (intptr_t)t, side_by_side);
  tb.panes.push_back(t);
  tb.term = t;
  layout_tab(tb);
  if (!t->start(s.cmd, s.cwd)) {
    close_pane(t);
    return;
  }
  t->show();
  t->take_focus();
  g.area->redraw();
}

static void close_pane(TermWidget *t) {
  int i = index_of(t);
  if (i < 0) return;
  Tab &tb = g.tabs[(size_t)i];
  if (tb.panes.size() <= 1) {
    close_tab(i);
    return;
  }
  // the focus goes to the pane that takes the place, or the first one
  tb.tree.remove((intptr_t)t);
  tb.panes.erase(std::remove(tb.panes.begin(), tb.panes.end(), t), tb.panes.end());
  if (tb.term == t) tb.term = (TermWidget *)tb.tree.ids().front();
  g.area->remove(t);
  Fl::delete_widget(t);
  layout_tab(tb);
  g.area->redraw();
  if (i == g.current) {
    tb.term->take_focus();
    update_title();
  }
}

static void focus_pane(term::Dir d) {
  if (g.tabs.empty()) return;
  Tab &tb = g.tabs[(size_t)g.current];
  auto id = tb.tree.neighbor((intptr_t)tb.term, d, area_rect(), PANE_GAP);
  if (id) ((TermWidget *)id)->take_focus();
}

// The terminals' group: paints the gaps between panes and drags them
class PaneArea : public Fl_Group {
public:
  PaneArea(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {}
  void draw() override {
    Fl_Group::draw();
    if (g.tabs.empty()) return;
    Tab &tb = g.tabs[(size_t)g.current];
    if (tb.panes.size() < 2) return;
    for (auto &r : tb.tree.dividers(area_rect(), PANE_GAP)) fl_rectf(r.x, r.y, r.w, r.h, fld::palette().bg);
    // the focused pane gets a line along its top
    fl_color(fld::palette().accent);
    fl_rectf(tb.term->x(), tb.term->y(), tb.term->w(), 2);
  }
  int handle(int e) override {
    if (!g.tabs.empty()) {
      Tab &tb = g.tabs[(size_t)g.current];
      bool vbar = false;
      switch (e) {
        case FL_PUSH:
          if (tb.tree.drag_begin(area_rect(), PANE_GAP, Fl::event_x(), Fl::event_y())) return 1;
          break;
        case FL_DRAG:
          if (tb.tree.dragging()) {
            tb.tree.drag_to(Fl::event_x(), Fl::event_y());
            layout_tab(tb);
            redraw();
            return 1;
          }
          break;
        case FL_RELEASE:
          if (tb.tree.dragging()) {
            tb.tree.drag_end();
            return 1;
          }
          break;
        case FL_MOVE:
        case FL_ENTER:
          if (tb.panes.size() > 1 && tb.tree.divider_at(area_rect(), PANE_GAP, Fl::event_x(), Fl::event_y(), &vbar)) {
            window()->cursor(vbar ? FL_CURSOR_WE : FL_CURSOR_NS);
            return 1;
          }
          window()->cursor(FL_CURSOR_DEFAULT);
          break;
      }
    }
    return Fl_Group::handle(e);
  }
};

// ---- search in the scrollback ----------------------------------------------------

static void search_bar(bool on) {
  if (on == (bool)g.search->visible()) {
    if (on) {
      g.search_in->take_focus();
      g.search_in->insert_position(0, g.search_in->size());
    }
    return;
  }
  if (on) g.search->show();
  else {
    g.search->hide();
    if (TermWidget *t = cur()) {
      t->clear_selection();
      t->scroll_to(0);
      t->take_focus();
    }
  }
  relayout();
  if (on) g.search_in->take_focus();
}

// older: towards the start of the scrollback
static void search_step(bool older) {
  TermWidget *t = cur();
  if (!t || !*g.search_in->value()) return;
  std::u32string needle = term::to_u32(g.search_in->value());
  auto lines = t->all_lines();
  long line;
  int col;
  if (!t->selection_start(line, col)) {
    // from the end of the text for older, from the start for newer
    line = older ? (long)lines.size() - 1 : 0;
    col = older ? (int)(lines.empty() ? 0 : lines.back().size() + 1) : -1;
  }
  term::Match m = term::find_in_lines(lines, needle, line, col, older, !g.search_case->value());
  if (m.line < 0) {
    g.search_msg->label("No matches");
    t->clear_selection();
    return;
  }
  g.search_msg->label("");
  t->show_match(m.line, m.col, m.len);
}

static void make_search_bar(int W, int H) {
  g.search = new Fl_Group(0, H - 34, W, 34);
  g.search->box(FL_FLAT_BOX);
  g.search->color(fld::palette().bg);
  g.search_in = new Fl_Input(8, H - 30, 200, 26);                         // 0
  g.search_in->when(FL_WHEN_ENTER_KEY_ALWAYS);
  g.search_in->callback([](Fl_Widget *, void *) { search_step(!(Fl::event_state() & FL_SHIFT)); });
  g.search_in->tooltip("Enter: older, Shift+Enter: newer, Escape: close");
  auto *prev = new Fl_Button(0, H - 30, 34, 26, "@8->");                  // 1
  prev->tooltip("Older (Enter)");
  prev->callback([](Fl_Widget *, void *) { search_step(true); });
  g.search_case = new fld::CheckButton(0, H - 30, 110, 26, "Match case");  // 2
  g.search_msg = new Fl_Box(0, H - 30, 120, 26);                           // 3
  g.search_msg->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  g.search_msg->labelcolor(fld::palette().fg_dim);
  auto *close = new Fl_Button(0, H - 30, 28, 26, "@1+");                  // 4
  close->label("\u00d7");
  close->tooltip("Close (Escape)");
  close->callback([](Fl_Widget *, void *) { search_bar(false); });
  auto *next = new Fl_Button(0, H - 30, 34, 26, "@2->");                  // 5
  next->tooltip("Newer (Shift+Enter)");
  next->callback([](Fl_Widget *, void *) { search_step(false); });
  g.search->end();
  g.search->hide();
}

// ---- profiles --------------------------------------------------------------------

static void save_profiles() {
  fld::Ini ini;
  ini.load(term::config_path());
  term::store_profiles(g.profiles, ini);
  term::store(g.set, ini);
  if (!ini.save(term::config_path())) fl_alert("Could not save %s", term::config_path().c_str());
}

namespace {
struct ProfileDlg {
  Fl_Hold_Browser *list;
  Fl_Input *name, *command, *cwd;
  Fl_Choice *scheme;
  Fl_Spinner *font;
  Fl_Check_Button *def;
  std::vector<term::Profile> ps;
  std::string def_name;
  int sel = -1;
};
const char *kProfileSchemes[] = {"", "dark", "light", "red", "green", "blue"};
} // namespace

static void profile_store(ProfileDlg &d) {
  if (d.sel < 0 || d.sel >= (int)d.ps.size()) return;
  term::Profile &p = d.ps[(size_t)d.sel];
  std::string n = fld::trim(d.name->value());
  if (!n.empty() && n.find(']') == std::string::npos) {
    if (d.def_name == p.name) d.def_name = n;
    p.name = n;
  }
  p.command = d.command->value();
  p.cwd = d.cwd->value();
  p.scheme = kProfileSchemes[std::max(0, d.scheme->value())];
  p.font_size = (int)d.font->value();
  if (d.def->value()) d.def_name = p.name;
  else if (d.def_name == p.name) d.def_name.clear();
}

static void profile_show(ProfileDlg &d) {
  d.list->clear();
  for (auto &p : d.ps) d.list->add(fld::escape_at(p.name + (p.name == d.def_name ? "  (default)" : "")).c_str());
  bool have = d.sel >= 0 && d.sel < (int)d.ps.size();
  for (Fl_Widget *w : std::initializer_list<Fl_Widget *>{d.name, d.command, d.cwd, d.scheme, d.font, d.def})
    if (have) w->activate(); else w->deactivate();
  if (!have) return;
  d.list->value(d.sel + 1);
  const term::Profile &p = d.ps[(size_t)d.sel];
  d.name->value(p.name.c_str());
  d.command->value(p.command.c_str());
  d.cwd->value(p.cwd.c_str());
  d.scheme->value(0);
  for (int k = 0; k < 6; k++)
    if (p.scheme == kProfileSchemes[k]) d.scheme->value(k);
  d.font->value(p.font_size);
  d.def->value(p.name == d.def_name);
}

static void profiles_dialog() {
  ProfileDlg d;
  load_profiles();
  d.ps = g.profiles;
  d.def_name = g.set.default_profile;
  d.sel = d.ps.empty() ? -1 : 0;
  const int W = 560, H = 330;
  Fl_Double_Window *w = new Fl_Double_Window(W, H, "Terminal Profiles");
  d.list = new Fl_Hold_Browser(12, 12, 170, H - 96);
  auto *add = new Fl_Button(12, H - 78, 82, 28, "Add");
  auto *del = new Fl_Button(100, H - 78, 82, 28, "Remove");
  int lx = 300, fw = W - lx - 12, y = 12;
  d.name = new Fl_Input(lx, y, fw, 28, "Name:");
  y += 36;
  d.command = new Fl_Input(lx, y, fw, 28, "Command:");
  d.command->tooltip("Empty: your shell. A command line, e.g. tmux new -A -s main");
  y += 36;
  d.cwd = new Fl_Input(lx, y, fw, 28, "Start in:");
  d.cwd->tooltip("Empty: the current tab's folder");
  y += 36;
  d.scheme = new Fl_Choice(lx, y, fw, 28, "Color scheme:");
  d.scheme->add("The general one|Dark|Light|Dark red|Dark green|Dark blue");
  y += 36;
  d.font = new Fl_Spinner(lx, y, 80, 28, "Font size:");
  d.font->range(0, 48);
  d.font->tooltip("0: the general size");
  y += 36;
  d.def = new fld::CheckButton(lx, y, fw, 28, "Use for new tabs and windows");
  auto *ok = new Fl_Return_Button(W - 112, H - 42, 100, 30, "Close");
  ok->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  d.list->callback([](Fl_Widget *, void *v) {
    ProfileDlg &d = *(ProfileDlg *)v;
    profile_store(d);
    d.sel = d.list->value() - 1;
    profile_show(d);
  }, &d);
  add->callback([](Fl_Widget *, void *v) {
    ProfileDlg &d = *(ProfileDlg *)v;
    profile_store(d);
    term::Profile p;
    for (int n = 1;; n++) {
      p.name = n == 1 ? "New Profile" : "New Profile " + std::to_string(n);
      if (!term::find_profile(d.ps, p.name)) break;
    }
    d.ps.push_back(p);
    d.sel = (int)d.ps.size() - 1;
    profile_show(d);
    d.name->take_focus();
  }, &d);
  del->callback([](Fl_Widget *, void *v) {
    ProfileDlg &d = *(ProfileDlg *)v;
    if (d.sel < 0) return;
    if (d.ps[(size_t)d.sel].name == d.def_name) d.def_name.clear();
    d.ps.erase(d.ps.begin() + d.sel);
    d.sel = std::min(d.sel, (int)d.ps.size() - 1);
    profile_show(d);
  }, &d);
  w->end();
  w->set_modal();
  profile_show(d);
  w->show();
  while (w->shown()) Fl::wait();
  profile_store(d);
  // names must be unique: later duplicates get a number
  for (size_t i = 0; i < d.ps.size(); i++)
    for (size_t k = 0; k < i; k++)
      if (d.ps[k].name == d.ps[i].name) d.ps[i].name += " " + std::to_string(i + 1);
  g.profiles = d.ps;
  g.set.default_profile = term::find_profile(g.profiles, d.def_name) ? d.def_name : "";
  save_profiles();
  Fl::delete_widget(w);
}

static void new_window() {
  TermWidget *t = cur();
  std::string dir = t ? t->current_dir() : "";
  std::vector<std::string> argv{"fl-term"};
  if (!dir.empty()) { argv.push_back("--workdir"); argv.push_back(dir); }
  fld::spawn(argv);
}

// SSH connections and helper commands open in a new tab of this window
static void open_ssh_tab(const std::vector<std::string> &cmd, const std::string &title,
                         const std::string &scheme, bool hold, bool hold_on_error) {
  TabSpec s;
  s.cmd = cmd;
  s.title = title;
  s.scheme = scheme;
  s.hold = hold;
  s.hold_on_error = hold_on_error;
  add_tab(s);
}

static void apply_settings() {
  for (auto &tb : g.tabs)
    for (TermWidget *t : tb.panes) {
      if (!tb.own_scheme) t->set_colors(*scheme_colors(g.set.scheme));
      t->set_font_size(g.set.font_size > 0 ? g.set.font_size : fld::mono_size());
      t->set_scrollback_limit((size_t)g.set.scrollback);
      t->set_opacity(g.set.opacity);
    }
  update_opaque_region();
  relayout();
}

static void save_settings() {
  if (!term::save_file(g.set)) fl_alert("Could not save %s", term::config_path().c_str());
}

static void toggle_menubar() {
  g.set.show_menubar = !g.set.show_menubar;
  relayout();
  save_settings();
}

static void set_tabs_mode(term::TabsMode m) {
  g.set.tabs = m;
  relayout();
  save_settings();
}

// ---- preferences ----------------------------------------------------------------

static void preferences_dialog() {
  static const char *schemes[] = {"dark", "light", "red", "green", "blue"};
  static const char *scheme_labels[] = {"Dark", "Light", "Dark red", "Dark green", "Dark blue"};
  Fl_Double_Window *d = new Fl_Double_Window(420, 300, "Terminal Preferences");
  int lx = 160, y = 16;
  Fl_Choice *sch = new Fl_Choice(lx, y, 240, 28, "Color scheme:");
  for (int i = 0; i < 5; i++) {
    sch->add(scheme_labels[i]);
    if (g.set.scheme == schemes[i]) sch->value(i);
  }
  if (sch->value() < 0) sch->value(0);
  y += 38;
  Fl_Spinner *font = new Fl_Spinner(lx, y, 80, 28, "Font size:");
  font->range(6, 48);
  font->value(g.set.font_size > 0 ? g.set.font_size : (cur() ? cur()->font_size() : fld::mono_size()));
  y += 38;
  Fl_Spinner *sb = new Fl_Spinner(lx, y, 110, 28, "Scrollback lines:");
  sb->range(0, 1000000);
  sb->step(1000);
  sb->value(g.set.scrollback);
  y += 38;
  Fl_Hor_Value_Slider *op = new Fl_Hor_Value_Slider(lx, y, 240, 28, "Background opacity:");
  op->align(FL_ALIGN_LEFT);
  op->range(20, 100);
  op->step(5);
  op->value((int)lround(g.set.opacity * 100));
  op->tooltip("Below 100% the desktop shows through the terminal background (in percent)");
  y += 38;
  Fl_Check_Button *mb = new fld::CheckButton(lx, y, 240, 28, "Show the menu bar");
  mb->value(g.set.show_menubar);
  y += 34;
  Fl_Choice *tabs = new Fl_Choice(lx, y, 240, 28, "Tab bar:");
  tabs->add("Show with two or more tabs");
  tabs->add("Always show");
  tabs->add("Never show");
  tabs->value((int)g.set.tabs);
  y += 52;
  Fl_Button *cancel = new Fl_Button(420 - 220, y, 100, 30, "Cancel");
  Fl_Return_Button *ok = new Fl_Return_Button(420 - 112, y, 100, 30, "OK");
  d->end();
  d->set_modal();
  fld::set_window_icon(d, "utilities-terminal");
  // live preview of the opacity
  op->callback([](Fl_Widget *w, void *) {
    double o = ((Fl_Valuator *)w)->value() / 100.0;
    for (auto &tb : g.tabs)
      for (TermWidget *t : tb.panes) t->set_opacity(o);
    double saved = g.set.opacity;
    g.set.opacity = o;
    update_opaque_region();
    g.set.opacity = saved;
  });
  int result = 0;
  cancel->callback([](Fl_Widget *w, void *r) { *(int *)r = 1; w->window()->hide(); }, &result);
  ok->callback([](Fl_Widget *w, void *r) { *(int *)r = 2; w->window()->hide(); }, &result);
  d->callback([](Fl_Widget *w, void *r) { *(int *)r = 1; w->hide(); }, &result);
  d->show();
  while (d->shown()) Fl::wait();
  if (result == 2) {
    g.set.scheme = schemes[std::max(0, sch->value())];
    g.set.font_size = (int)font->value();
    g.set.scrollback = (int)sb->value();
    g.set.opacity = term::clamp_opacity(op->value() / 100.0);
    g.set.show_menubar = mb->value();
    g.set.tabs = (term::TabsMode)std::max(0, tabs->value());
    save_settings();
  }
  apply_settings(); // also undoes the opacity preview on cancel
  Fl::delete_widget(d);
}

// ---- menus ----------------------------------------------------------------------

enum MenuId {
  M_COPY = 1, M_PASTE, M_NEW_WINDOW, M_ZOOM_IN, M_ZOOM_OUT, M_ZOOM_RESET, M_OPEN_FOLDER, M_ABOUT,
  M_CLOSE_WINDOW, M_NEW_TAB, M_CLOSE_TAB, M_NEXT_TAB, M_PREV_TAB, M_MOVE_LEFT, M_MOVE_RIGHT,
  M_TOGGLE_MENUBAR, M_TABS_AUTO, M_TABS_ALWAYS, M_TABS_NEVER, M_PREFS,
  M_SPLIT_H, M_SPLIT_V, M_CLOSE_PANE, M_FIND, M_PROFILES,
  M_PROFILE = 100, // + index: New Tab with that profile
  M_SSH_CONNECT = 40, M_SSH_MANAGE, M_SSH_COPY_KEY, M_SSH_GEN_KEY, M_SSH_BROWSE,
};

static void menu_action(long id) {
  TermWidget *t = cur();
  switch (id) {
    case M_COPY: if (t) t->copy_selection(1); break;
    case M_PASTE: if (t) t->paste(1); break;
    case M_NEW_WINDOW: new_window(); break;
    case M_NEW_TAB: new_tab(); break;
    case M_CLOSE_TAB: close_tab(g.current); break;
    case M_NEXT_TAB: select_tab(g.current + 1); break;
    case M_PREV_TAB: select_tab(g.current - 1); break;
    case M_MOVE_LEFT: move_tab(-1); break;
    case M_MOVE_RIGHT: move_tab(1); break;
    case M_ZOOM_IN: if (t) t->set_font_size(t->font_size() + 1); break;
    case M_ZOOM_OUT: if (t) t->set_font_size(t->font_size() - 1); break;
    case M_ZOOM_RESET: if (t) t->set_font_size(g.set.font_size > 0 ? g.set.font_size : fld::mono_size()); break;
    case M_OPEN_FOLDER: {
      std::string d = t ? t->current_dir() : "";
      fld::spawn({"fl-files", d.empty() ? fld::home_dir() : d});
      break;
    }
    case M_ABOUT: fld::about_dialog("Terminal", "utilities-terminal",
                                    "A terminal emulator built with FLTK and libvterm."); break;
    case M_CLOSE_WINDOW: g.win->hide(); break;
    case M_TOGGLE_MENUBAR: toggle_menubar(); break;
    case M_TABS_AUTO: set_tabs_mode(term::TABS_AUTO); break;
    case M_TABS_ALWAYS: set_tabs_mode(term::TABS_ALWAYS); break;
    case M_TABS_NEVER: set_tabs_mode(term::TABS_NEVER); break;
    case M_PREFS: preferences_dialog(); break;
    case M_SPLIT_H: split_pane(true); break;
    case M_SPLIT_V: split_pane(false); break;
    case M_CLOSE_PANE: if (t) close_pane(t); break;
    case M_FIND: search_bar(true); break;
    case M_PROFILES: profiles_dialog(); break;
    default:
      if (id >= M_PROFILE && id < M_PROFILE + (long)g.profiles.size()) new_tab(g.profiles[(size_t)(id - M_PROFILE)].name);
      break;
    case M_SSH_CONNECT: ssh_ui::connect_dialog(); break;
    case M_SSH_MANAGE: ssh_ui::manage_dialog(); break;
    case M_SSH_COPY_KEY: ssh_ui::copy_key_dialog(); break;
    case M_SSH_GEN_KEY: ssh_ui::generate_key(); break;
    case M_SSH_BROWSE: ssh_ui::browse_files_dialog(); break;
  }
}

static void menu_cb(Fl_Widget *, void *v) {
  // run after the menu has closed: some actions open dialogs or delete tabs
  Fl::add_timeout(0, [](void *id) { menu_action((long)id); }, v);
}

static std::vector<ssh::Profile> g_menu_profiles;
static std::vector<std::string> g_menu_mounts;
static std::vector<std::string> g_menu_labels; // must outlive the menus

static void ssh_profile_cb(Fl_Widget *, void *v) {
  long i = (long)v;
  if (i >= 0 && i < (long)g_menu_profiles.size()) ssh_ui::connect(g_menu_profiles[(size_t)i]);
}

static void unmount_cb(Fl_Widget *, void *v) {
  long i = (long)v;
  if (i >= 0 && i < (long)g_menu_mounts.size()) ssh_ui::unmount(g_menu_mounts[(size_t)i]);
}

static std::string menu_escape(const std::string &t) {
  std::string e;
  for (char c : t) {
    if (c == '/' || c == '\\' || c == '&' || c == '_' || c == '@') e += '\\';
    e += c;
  }
  return e;
}

typedef std::vector<Fl_Menu_Item> Items;
static void item(Items &m, const char *label, int shortcut, long id, int flags = 0) {
  m.push_back({label, shortcut, menu_cb, (void *)id, flags});
}
static void end_menu(Items &m) { m.push_back({nullptr}); }

// the SSH submenu, the same in the menu bar and the context menu
static void ssh_items(Items &m) {
  g_menu_profiles = ssh_ui::profiles();
  g_menu_mounts = ssh_ui::sshfs_mounts();
  item(m, "Connect…", FL_CTRL + FL_SHIFT + 's', M_SSH_CONNECT, FL_MENU_DIVIDER);
  for (size_t i = 0; i < g_menu_profiles.size() && i < 30; i++) {
    g_menu_labels.push_back(menu_escape(g_menu_profiles[i].name));
    m.push_back({g_menu_labels.back().c_str(), 0, ssh_profile_cb, (void *)(long)i});
  }
  if (!g_menu_profiles.empty()) m.back().flags |= FL_MENU_DIVIDER;
  item(m, "Manage Connections…", 0, M_SSH_MANAGE);
  item(m, "Copy Public Key to Host…", 0, M_SSH_COPY_KEY);
  item(m, "Create SSH Key…", 0, M_SSH_GEN_KEY);
  if (ssh_ui::have_sshfs()) {
    m.back().flags |= FL_MENU_DIVIDER;
    item(m, "Browse Files (sshfs)…", 0, M_SSH_BROWSE);
    if (!g_menu_mounts.empty()) {
      m.push_back({"Unmount", 0, nullptr, nullptr, FL_SUBMENU});
      for (size_t i = 0; i < g_menu_mounts.size(); i++) {
        g_menu_labels.push_back(menu_escape(fld::basename(g_menu_mounts[i])));
        m.push_back({g_menu_labels.back().c_str(), 0, unmount_cb, (void *)(long)i});
      }
      end_menu(m);
    }
  }
}

static void view_items(Items &m) {
  item(m, "Show Menu Bar", FL_CTRL + FL_SHIFT + 'm', M_TOGGLE_MENUBAR,
       FL_MENU_TOGGLE | (g.set.show_menubar ? FL_MENU_VALUE : 0));
  m.push_back({"Show Tabs", 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER});
  item(m, "With Two or More Tabs", 0, M_TABS_AUTO, FL_MENU_RADIO | (g.set.tabs == term::TABS_AUTO ? FL_MENU_VALUE : 0));
  item(m, "Always", 0, M_TABS_ALWAYS, FL_MENU_RADIO | (g.set.tabs == term::TABS_ALWAYS ? FL_MENU_VALUE : 0));
  item(m, "Never", 0, M_TABS_NEVER, FL_MENU_RADIO | (g.set.tabs == term::TABS_NEVER ? FL_MENU_VALUE : 0));
  end_menu(m);
  item(m, "Zoom In", FL_CTRL + '=', M_ZOOM_IN);
  item(m, "Zoom Out", FL_CTRL + '-', M_ZOOM_OUT);
  item(m, "Normal Size", FL_CTRL + '0', M_ZOOM_RESET);
}

static void pane_items(Items &m) {
  bool split = !g.tabs.empty() && g.tabs[(size_t)g.current].panes.size() > 1;
  item(m, "Split Side by Side", FL_CTRL + FL_SHIFT + 'e', M_SPLIT_H);
  item(m, "Split Above and Below", FL_CTRL + FL_SHIFT + 'o', M_SPLIT_V);
  item(m, "Close Pane", split ? FL_CTRL + FL_SHIFT + 'w' : 0, M_CLOSE_PANE, split ? 0 : FL_MENU_INACTIVE);
}

static void profile_items(Items &m) {
  load_profiles();
  if (!g.profiles.empty()) {
    m.push_back({"New Tab with Profile", 0, nullptr, nullptr, FL_SUBMENU});
    for (size_t i = 0; i < g.profiles.size() && i < 30; i++) {
      g_menu_labels.push_back(menu_escape(g.profiles[i].name));
      m.push_back({g_menu_labels.back().c_str(), 0, menu_cb, (void *)(long)(M_PROFILE + (long)i)});
    }
    end_menu(m);
  }
  item(m, "Profiles…", 0, M_PROFILES, FL_MENU_DIVIDER);
}

static void tab_items(Items &m) {
  bool many = g.tabs.size() > 1;
  item(m, "Next Tab", FL_CTRL + FL_Page_Down, M_NEXT_TAB, many ? 0 : FL_MENU_INACTIVE);
  item(m, "Previous Tab", FL_CTRL + FL_Page_Up, M_PREV_TAB, (many ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER);
  item(m, "Move Tab Left", FL_CTRL + FL_SHIFT + FL_Page_Up, M_MOVE_LEFT, g.current > 0 ? 0 : FL_MENU_INACTIVE);
  item(m, "Move Tab Right", FL_CTRL + FL_SHIFT + FL_Page_Down, M_MOVE_RIGHT,
       g.current + 1 < (int)g.tabs.size() ? 0 : FL_MENU_INACTIVE);
}

static Items build_menubar_items() {
  g_menu_labels.clear();
  g_menu_labels.reserve(128);
  Items m;
  m.push_back({"&File", 0, nullptr, nullptr, FL_SUBMENU});
  item(m, "New Tab", FL_CTRL + FL_SHIFT + 't', M_NEW_TAB);
  item(m, "New Window", FL_CTRL + FL_SHIFT + 'n', M_NEW_WINDOW, FL_MENU_DIVIDER);
  profile_items(m);
  pane_items(m);
  m.back().flags |= FL_MENU_DIVIDER;
  item(m, "Open Folder", 0, M_OPEN_FOLDER, FL_MENU_DIVIDER);
  item(m, "Close Tab", 0, M_CLOSE_TAB);
  item(m, "Close Window", FL_CTRL + FL_SHIFT + 'q', M_CLOSE_WINDOW);
  end_menu(m);
  m.push_back({"&Edit", 0, nullptr, nullptr, FL_SUBMENU});
  item(m, "Copy", FL_CTRL + FL_SHIFT + 'c', M_COPY);
  item(m, "Paste", FL_CTRL + FL_SHIFT + 'v', M_PASTE, FL_MENU_DIVIDER);
  item(m, "Find…", FL_CTRL + FL_SHIFT + 'f', M_FIND, FL_MENU_DIVIDER);
  item(m, "Preferences…", FL_CTRL + FL_SHIFT + 'p', M_PREFS);
  end_menu(m);
  m.push_back({"&View", 0, nullptr, nullptr, FL_SUBMENU});
  view_items(m);
  end_menu(m);
  m.push_back({"&Tabs", 0, nullptr, nullptr, FL_SUBMENU});
  tab_items(m);
  end_menu(m);
  m.push_back({"&SSH", 0, nullptr, nullptr, FL_SUBMENU});
  ssh_items(m);
  end_menu(m);
  m.push_back({"&Help", 0, nullptr, nullptr, FL_SUBMENU});
  item(m, "About", 0, M_ABOUT);
  end_menu(m);
  end_menu(m);
  return m;
}

// The menu bar rebuilds its menus when opened (SSH profiles and mounts change).
class TermMenuBar : public Fl_Menu_Bar {
public:
  using Fl_Menu_Bar::Fl_Menu_Bar;
  void rebuild() {
    items_ = build_menubar_items();
    menu(items_.data());
  }
  int handle(int e) override {
    if (e == FL_PUSH) rebuild();
    // shortcuts are handled by global_shortcuts()
    if (e == FL_SHORTCUT) return 0;
    return Fl_Menu_Bar::handle(e);
  }

private:
  Items items_;
};

static void show_menu(TermWidget *t, void *) {
  // a right click in a background tab is not possible; still make sure the
  // clicked terminal is the current one
  int i = index_of(t);
  if (i >= 0 && i != g.current) select_tab(i);
  g_menu_labels.clear();
  g_menu_labels.reserve(128);
  Items m;
  item(m, "Copy", FL_CTRL + FL_SHIFT + 'c', M_COPY);
  item(m, "Paste", FL_CTRL + FL_SHIFT + 'v', M_PASTE, FL_MENU_DIVIDER);
  item(m, "New Tab", FL_CTRL + FL_SHIFT + 't', M_NEW_TAB);
  item(m, "New Window", FL_CTRL + FL_SHIFT + 'n', M_NEW_WINDOW);
  item(m, "Find…", FL_CTRL + FL_SHIFT + 'f', M_FIND, FL_MENU_DIVIDER);
  pane_items(m);
  m.back().flags |= FL_MENU_DIVIDER;
  m.push_back({"Profiles", 0, nullptr, nullptr, FL_SUBMENU});
  profile_items(m);
  end_menu(m);
  item(m, "Open Folder", 0, M_OPEN_FOLDER, FL_MENU_DIVIDER);
  m.push_back({"SSH", 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER});
  ssh_items(m);
  end_menu(m);
  if (g.tabs.size() > 1) {
    m.push_back({"Tabs", 0, nullptr, nullptr, FL_SUBMENU});
    tab_items(m);
    end_menu(m);
  }
  m.push_back({"View", 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER});
  view_items(m);
  end_menu(m);
  item(m, "Preferences…", FL_CTRL + FL_SHIFT + 'p', M_PREFS);
  item(m, "About", 0, M_ABOUT, FL_MENU_DIVIDER);
  if (g.tabs.size() > 1) item(m, "Close Tab", FL_CTRL + FL_SHIFT + 'w', M_CLOSE_TAB);
  item(m, "Close Window", FL_CTRL + FL_SHIFT + 'q', M_CLOSE_WINDOW);
  end_menu(m);
  const Fl_Menu_Item *r = m[0].popup(Fl::event_x(), Fl::event_y());
  if (r && r->callback()) r->do_callback(g.win);
}

// ---- the tab bar's menu -------------------------------------------------------------

static void duplicate_tab(int i) {
  if (i < 0 || i >= (int)g.tabs.size()) return;
  TabSpec s = g.tabs[(size_t)i].spec;
  std::string dir = g.tabs[(size_t)i].term->current_dir();
  if (s.cmd.empty() && !dir.empty()) s.cwd = dir; // a shell: in its current folder
  if (TermWidget *t = add_tab(s)) {
    // next to the original
    int at = index_of(t);
    reorder_tab(at, std::min(i + 1, (int)g.tabs.size() - 1));
    select_tab(index_of(t));
  }
}

static void rename_tab(int i) {
  if (i < 0 || i >= (int)g.tabs.size()) return;
  Tab &tb = g.tabs[(size_t)i];
  std::string cur_name = !tb.custom.empty() ? tb.custom : tb.term->title();
  const char *r = fl_input("Tab name (empty: the terminal's own title):", cur_name.c_str());
  if (!r) return;
  // the tab may have closed while the dialog was open
  for (auto &t : g.tabs)
    if (&t == &tb) {
      t.custom = fld::trim(r);
      update_title();
    }
}

// opens a new window with what the tab runs, then closes the tab
static void tab_to_window(int i) {
  if (i < 0 || i >= (int)g.tabs.size() || g.tabs.size() < 2) return;
  Tab &tb = g.tabs[(size_t)i];
  std::vector<std::string> argv{"fl-term"};
  std::string dir = tb.term->current_dir();
  if (!dir.empty()) { argv.push_back("--workdir"); argv.push_back(dir); }
  if (!tb.spec.title.empty()) { argv.push_back("--title"); argv.push_back(tb.spec.title); }
  if (!tb.spec.scheme.empty()) { argv.push_back("--scheme"); argv.push_back(tb.spec.scheme); }
  if (tb.spec.hold_on_error) argv.push_back("--hold-on-error");
  if (!tb.spec.cmd.empty()) {
    argv.push_back("-e");
    for (auto &a : tb.spec.cmd) argv.push_back(a);
  }
  // a shell's state cannot move: ask before ending a running program
  if (tb.term->has_children() &&
      fl_choice("A program is still running in this tab. It ends when the tab moves to a new window.",
                "Cancel", "Move", nullptr) != 1)
    return;
  fld::spawn(argv);
  close_tab(i);
}

// ---- moving tabs to other fl-term windows: the new window starts the same
// command in the same folder (a running program cannot move along)

static std::string export_tab(int i) {
  if (i < 0 || i >= (int)g.tabs.size()) return "";
  Tab &tb = g.tabs[(size_t)i];
  fld::Json j = fld::Json::object();
  fld::Json cmd = fld::Json::array();
  for (auto &a : tb.spec.cmd) cmd.push(a);
  j.set("cmd", cmd);
  std::string dir = tb.term->current_dir();
  j.set("cwd", dir.empty() ? tb.spec.cwd : dir);
  j.set("title", tb.spec.title);
  j.set("scheme", tb.spec.scheme);
  j.set("hold", tb.spec.hold);
  j.set("hold_on_error", tb.spec.hold_on_error);
  j.set("custom", tb.custom);
  return j.dump();
}

static bool spec_from_state(const std::string &state, TabSpec &spec, std::string &custom) {
  fld::Json j;
  if (!fld::Json::parse(state, j) || !j.is_object()) return false;
  for (auto &a : j["cmd"].items()) spec.cmd.push_back(a.as_string());
  spec.cwd = j["cwd"].as_string();
  spec.title = j["title"].as_string();
  spec.scheme = j["scheme"].as_string();
  spec.hold = j["hold"].as_bool();
  spec.hold_on_error = j["hold_on_error"].as_bool();
  custom = j["custom"].as_string();
  return true;
}

static bool import_tab(const std::string &state, int at) {
  TabSpec spec;
  std::string custom;
  if (!spec_from_state(state, spec, custom)) return false;
  TermWidget *t = add_tab(spec);
  if (!t) return false;
  int i = index_of(t);
  g.tabs[(size_t)i].custom = custom;
  int to = std::max(0, std::min((int)g.tabs.size() - 1, at));
  if (to != i) reorder_tab(i, to);
  select_tab(index_of(t));
  g.win->show();
  return true;
}

// the tab now runs in another window: close it here (ask first when a
// program still runs in it)
static void tab_moved_away(int i) {
  if (i < 0 || i >= (int)g.tabs.size()) return;
  if (g.tabs[(size_t)i].term->has_children() &&
      fl_choice("The tab was opened in the other window. A program is still running in this one; close it?",
                "Keep It", "Close", nullptr) != 1)
    return;
  close_tab(i);
}

static void close_others(int keep) {
  if (keep < 0 || keep >= (int)g.tabs.size()) return;
  TermWidget *k = g.tabs[(size_t)keep].term;
  for (int i = (int)g.tabs.size() - 1; i >= 0; i--)
    if (g.tabs[(size_t)i].term != k) close_tab(i);
}

static void close_right(int from) {
  for (int i = (int)g.tabs.size() - 1; i > from; i--) close_tab(i);
}

static void tab_menu(int i) {
  int n = (int)g.tabs.size();
  fld::Menu m;
  m.item("New Tab", [] { new_tab(); }, 0, FL_CTRL + FL_SHIFT + 't');
  if (i >= 0) {
    if (i != g.current) select_tab(i);
    m.item("Duplicate Tab", [i] { duplicate_tab(i); });
    m.item("Rename Tab…", [i] { Fl::add_timeout(0, [](void *v) { rename_tab((int)(long)v); }, (void *)(long)i); });
    m.divider();
    m.item("Move Tab Left", [i] { reorder_tab(i, i - 1); }, i > 0 ? 0 : FL_MENU_INACTIVE);
    m.item("Move Tab Right", [i] { reorder_tab(i, i + 1); }, i + 1 < n ? 0 : FL_MENU_INACTIVE);
    m.item("Move to New Window", [i] { Fl::add_timeout(0, [](void *v) { tab_to_window((int)(long)v); }, (void *)(long)i); },
           n > 1 ? 0 : FL_MENU_INACTIVE);
    m.divider();
    m.item("Open Folder", [i] {
      std::string d = g.tabs[(size_t)i].term->current_dir();
      fld::spawn({"fl-files", d.empty() ? fld::home_dir() : d});
    });
    m.divider();
    m.item("Close Tab", [i] { Fl::add_timeout(0, [](void *v) { close_tab((int)(long)v); }, (void *)(long)i); }, 0,
           FL_CTRL + FL_SHIFT + 'w');
    m.item("Close Other Tabs", [i] { Fl::add_timeout(0, [](void *v) { close_others((int)(long)v); }, (void *)(long)i); },
           n > 1 ? 0 : FL_MENU_INACTIVE);
    m.item("Close Tabs to the Right", [i] { Fl::add_timeout(0, [](void *v) { close_right((int)(long)v); }, (void *)(long)i); },
           i + 1 < n ? 0 : FL_MENU_INACTIVE);
  } else {
    m.item("New Window", [] { new_window(); }, 0, FL_CTRL + FL_SHIFT + 'n');
  }
  m.popup();
}

static int global_shortcuts(int e) {
  if (e != FL_SHORTCUT && e != FL_KEYBOARD) return 0;
  // only for the terminal window (not for dialogs)
  if (Fl::first_window() != g.win) return 0;
  int st = Fl::event_state();
  int key = Fl::event_key();
  bool ctrl = st & FL_CTRL, shift = st & FL_SHIFT, alt = st & FL_ALT;
  if (ctrl && shift && !alt) {
    switch (key) {
      case 'n': new_window(); return 1;
      case 't': new_tab(); return 1;
      case 'w':
        if (!g.tabs.empty() && g.tabs[(size_t)g.current].panes.size() > 1) close_pane(cur());
        else close_tab(g.current);
        return 1;
      case 'e': split_pane(true); return 1;
      case 'o': split_pane(false); return 1;
      case 'f': search_bar(true); return 1;
      case FL_Left: focus_pane(term::Dir::Left); return 1;
      case FL_Right: focus_pane(term::Dir::Right); return 1;
      case FL_Up: focus_pane(term::Dir::Up); return 1;
      case FL_Down: focus_pane(term::Dir::Down); return 1;
      case 'q': g.win->hide(); return 1;
      case 's': ssh_ui::connect_dialog(); return 1;
      case 'm': toggle_menubar(); return 1;
      case 'p': preferences_dialog(); return 1;
      case FL_Page_Up: move_tab(-1); return 1;
      case FL_Page_Down: move_tab(1); return 1;
    }
  }
  if (key == FL_Escape && !ctrl && !alt && g.search->visible() && Fl::focus() &&
      Fl::focus()->inside(g.search)) {
    search_bar(false);
    return 1;
  }
  if (ctrl && !shift && !alt) {
    if (key == FL_Page_Up) { select_tab(g.current - 1); return 1; }
    if (key == FL_Page_Down) { select_tab(g.current + 1); return 1; }
  }
  // Alt+1..9 picks a tab when there are several
  if (alt && !ctrl && !shift && key >= '1' && key <= '9' && g.tabs.size() > 1) {
    int i = key - '1';
    if (i < (int)g.tabs.size()) { select_tab(i); return 1; }
  }
  return 0;
}

static void close_cb(Fl_Widget *w, void *) { w->hide(); }

int main(int argc, char **argv) {
  fld::init_app("fl-term", argc, argv);
  std::vector<std::string> cmd;
  std::string workdir, title, ssh_arg, scheme_arg, adopted_name, profile_arg;
  int cols = 90, rows = 26;
  bool hold = false, hold_on_error = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-e" || a == "--command" || a == "-x") {
      for (int j = i + 1; j < argc; j++) cmd.push_back(argv[j]);
      // a single argument with spaces is a shell command line
      if (cmd.size() == 1 && cmd[0].find(' ') != std::string::npos) cmd = {"/bin/sh", "-c", cmd[0]};
      break;
    } else if ((a == "-d" || a == "--workdir" || a == "--working-directory") && i + 1 < argc) {
      workdir = argv[++i];
    } else if (fld::starts_with(a, "--working-directory=")) {
      workdir = a.substr(20);
    } else if ((a == "-T" || a == "--title") && i + 1 < argc) {
      title = argv[++i];
    } else if ((a == "-g" || a == "--geometry") && i + 1 < argc) {
      sscanf(argv[++i], "%dx%d", &cols, &rows);
    } else if (a == "--ssh" && i + 1 < argc) {
      ssh_arg = argv[++i];
    } else if (fld::starts_with(a, "--ssh=")) {
      ssh_arg = a.substr(6);
    } else if (a == "--scheme" && i + 1 < argc) {
      scheme_arg = argv[++i];
    } else if (a == "--profile" && i + 1 < argc) {
      profile_arg = argv[++i];
    } else if (a == "--adopt-tab" && i + 1 < argc) {
      // a tab dragged out of another window
      std::string state;
      TabSpec spec;
      if (fld::claim_tab_file(argv[++i], "fl-term", state) && spec_from_state(state, spec, adopted_name)) {
        cmd = spec.cmd;
        workdir = spec.cwd;
        title = spec.title;
        scheme_arg = spec.scheme;
        hold = spec.hold;
        hold_on_error = spec.hold_on_error;
      }
    } else if (a == "--hold-on-error") {
      hold_on_error = true;
    } else if (a == "--hold") {
      hold = true;
    } else if (a == "-h" || a == "--help") {
      fputs(kUsage, stdout);
      return 0;
    } else if (a == "-v" || a == "--version") {
      printf("fl-term %s\n", FLD_VERSION);
      return 0;
    } else {
      fprintf(stderr, "fl-term: unknown option %s\n%s", argv[i], kUsage);
      return 1;
    }
  }

  g.set = term::load_file();
  load_profiles();
  if (!profile_arg.empty() && !term::find_profile(g.profiles, profile_arg)) {
    fprintf(stderr, "fl-term: no profile \"%s\"\n", profile_arg.c_str());
    return 1;
  }
  std::string scheme_override;
  if (!ssh_arg.empty()) {
    ssh::Profile p = ssh_ui::resolve(ssh_arg);
    if (p.host.empty()) {
      fprintf(stderr, "fl-term: no host in \"%s\"\n", ssh_arg.c_str());
      return 1;
    }
    cmd = ssh::build_argv(p);
    if (title.empty()) title = p.target();
    if (!p.scheme.empty() && scheme_arg.empty()) scheme_arg = p.scheme;
    hold_on_error = true; // show why a connection failed
  }
  if (!scheme_arg.empty()) scheme_override = scheme_arg;

  g.win = new MainWindow(800, 500, title.empty() ? "Terminal" : title.c_str());
  g.fixed_title = title;
  g.win->box(FL_NO_BOX); // the bars and terminals cover it; keeps the background translucent
  g.menubar = new TermMenuBar(0, 0, 800, 26);
  static_cast<TermMenuBar *>(g.menubar)->rebuild();
  g.menubar->box(FL_FLAT_BOX);
  g.tabbar = new fld::TabBar(0, 0, 800, fld::TabBar::preferred_height());
  g.tabbar->empty_label = "Terminal";
  g.tabbar->on_select = [](int i) { select_tab(i); };
  // deferred: the tab bar is in its own event handler
  g.tabbar->on_close = [](int i) { Fl::add_timeout(0, [](void *v) { close_tab((int)(long)v); }, (void *)(long)i); };
  g.tabbar->on_new = [] { new_tab(); };
  g.tabbar->on_move = [](int from, int to) { reorder_tab(from, to); };
  g.tabbar->on_menu = [](int i) { tab_menu(i); };
  g.tabbar->on_detach = [](int i) { Fl::add_timeout(0, [](void *v) { tab_to_window((int)(long)v); }, (void *)(long)i); };
  g.tabbar->app_id = "fl-term";
  g.tabbar->on_export = [](int i) { return export_tab(i); };
  g.tabbar->on_import = [](const std::string &state, int at, bool) { return import_tab(state, at); };
  g.tabbar->on_moved_away = [](int i) {
    Fl::add_timeout(0, [](void *v) { tab_moved_away((int)(long)v); }, (void *)(long)i);
  };
  g.tabbar->on_tear_off = [](int i, const std::string &file) {
    // same as Move to New Window, with the tab's state
    if (i >= 0 && i < (int)g.tabs.size() && g.tabs[(size_t)i].term->has_children() &&
        fl_choice("A program is still running in this tab. It ends when the tab moves to a new window.",
                  "Cancel", "Move", nullptr) != 1) {
      fld::discard_tab_file(file);
      return;
    }
    if (fld::spawn({fld::self_program("fl-term"), "--adopt-tab", file}))
      Fl::add_timeout(0, [](void *v) { close_tab((int)(long)v); }, (void *)(long)i);
    else fld::discard_tab_file(file);
  };
  g.area = new PaneArea(0, 0, 800, 500);
  g.area->box(FL_NO_BOX);
  g.area->end();
  make_search_bar(800, 500);
  g.win->resizable(g.area);
  g.win->callback(close_cb);
  g.win->end();
  ssh_ui::open_in_tab = open_ssh_tab;

  TabSpec first;
  first.cmd = cmd;
  first.cwd = workdir.empty() ? "" : fld::expand_tilde(workdir);
  first.scheme = scheme_override;
  first.hold = hold;
  first.hold_on_error = hold_on_error;
  // a profile for a plain start (no command, SSH or adopted tab)
  if (!profile_arg.empty()) apply_profile(first, profile_arg);
  else if (cmd.empty() && ssh_arg.empty() && adopted_name.empty() && !g.set.default_profile.empty())
    apply_profile(first, g.set.default_profile);
  g.area->begin();
  // the first terminal is created before the window is shown so the window
  // gets the size for cols x rows
  TermWidget *t = new TermWidget(0, 0, 800, 500);
  g.area->end();
  TabSpec look = first;
  look.title.clear();
  setup_term(t, look);
  g.tabs.push_back({t, !scheme_override.empty() || !first.profile.empty(), first, adopted_name});
  g.tabs.back().panes = {t};
  g.tabs.back().tree.reset((intptr_t)t);
  g.current = 0;

  int ww, wh;
  t->size_for(cols, rows, ww, wh);
  int extra = (g.set.show_menubar ? std::max(24, fld::ui_font_size() + 12) : 0) +
              (term::tabs_visible(g.set.tabs, 1) ? fld::TabBar::preferred_height() : 0);
  g.win->size(ww, wh + extra);
  relayout();
  update_title();
  fld::set_window_icon(g.win, "utilities-terminal");
  Fl::add_handler(global_shortcuts);

  g.win->show();
  update_opaque_region();
  if (!t->start(first.cmd, first.cwd)) {
    fl_alert("Could not start the terminal process.");
    return 1;
  }
  t->take_focus();
  return Fl::run();
}
