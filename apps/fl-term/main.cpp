// fl-term: terminal emulator for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Hor_Value_Slider.H>
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
    "  -v, --version           show the version\n"
    "Tabs: Ctrl+Shift+T new tab, Ctrl+Shift+W close tab, Ctrl+PgUp/PgDn switch tabs.\n"
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
  std::string cwd, title, scheme;
  bool hold = false, hold_on_error = false;
};

struct Tab {
  TermWidget *term;
  bool own_scheme; // keeps its colour scheme (SSH profiles, --scheme)
  TabSpec spec;       // how it was started (Duplicate Tab)
  std::string custom; // a name given with Rename Tab (wins over the title)
};

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
    if (g.tabs[i].term == t) return (int)i;
  return -1;
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
  g.area->resize(0, y, W, std::max(1, H - y));
  for (auto &tb : g.tabs) tb.term->resize(0, y, W, std::max(1, H - y));
  TermWidget *t = cur();
  if (t) {
    g.win->size_range(t->cell_w() * 10, t->cell_h() * 3 + y, 0, 0, t->cell_w(), t->cell_h());
  }
  g.win->redraw();
}

class MainWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  void resize(int x, int y, int w, int h) override {
    // children are placed by relayout(), not by FLTK's proportional resizing
    Fl_Widget::resize(x, y, w, h);
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
  for (size_t k = 0; k < g.tabs.size(); k++) {
    if ((int)k == i) g.tabs[k].term->show();
    else g.tabs[k].term->hide();
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
  TermWidget *t = g.tabs[(size_t)i].term;
  g.tabs.erase(g.tabs.begin() + i);
  g.area->remove(t);
  Fl::delete_widget(t); // the destructor ends the child process
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

static void on_exit(TermWidget *t, int, void *) {
  // closing from inside the terminal's own callback: defer
  Fl::add_timeout(0, [](void *v) { close_tab(index_of((TermWidget *)v)); }, t);
}

static void show_menu(TermWidget *, void *);


static TermWidget *add_tab(const TabSpec &spec) {
  g.area->begin();
  TermWidget *t = new TermWidget(g.area->x(), g.area->y(), g.area->w(), g.area->h());
  g.area->end();
  if (g.set.font_size > 0) t->set_font_size(g.set.font_size);
  t->set_colors(*scheme_colors(spec.scheme.empty() ? g.set.scheme : spec.scheme));
  t->set_opacity(g.set.opacity);
  t->set_scrollback_limit((size_t)g.set.scrollback);
  t->set_hold(spec.hold);
  t->set_hold_on_error(spec.hold_on_error);
  t->on_title = on_title;
  t->on_exit = on_exit;
  t->on_menu = show_menu;
  if (!spec.title.empty()) t->set_title(spec.title);
  g.tabs.push_back({t, !spec.scheme.empty(), spec, ""});
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

static void new_tab() {
  TabSpec s;
  if (TermWidget *t = cur()) s.cwd = t->current_dir();
  add_tab(s);
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
  for (auto &tb : g.tabs) {
    if (!tb.own_scheme) tb.term->set_colors(*scheme_colors(g.set.scheme));
    tb.term->set_font_size(g.set.font_size > 0 ? g.set.font_size : fld::mono_size());
    tb.term->set_scrollback_limit((size_t)g.set.scrollback);
    tb.term->set_opacity(g.set.opacity);
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
  Fl_Check_Button *mb = new Fl_Check_Button(lx, y, 240, 28, "Show the menu bar");
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
    for (auto &tb : g.tabs) tb.term->set_opacity(o);
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
  item(m, "Open Folder", 0, M_OPEN_FOLDER, FL_MENU_DIVIDER);
  item(m, "Close Tab", FL_CTRL + FL_SHIFT + 'w', M_CLOSE_TAB);
  item(m, "Close Window", FL_CTRL + FL_SHIFT + 'q', M_CLOSE_WINDOW);
  end_menu(m);
  m.push_back({"&Edit", 0, nullptr, nullptr, FL_SUBMENU});
  item(m, "Copy", FL_CTRL + FL_SHIFT + 'c', M_COPY);
  item(m, "Paste", FL_CTRL + FL_SHIFT + 'v', M_PASTE, FL_MENU_DIVIDER);
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
      case 'w': close_tab(g.current); return 1;
      case 'q': g.win->hide(); return 1;
      case 's': ssh_ui::connect_dialog(); return 1;
      case 'm': toggle_menubar(); return 1;
      case 'p': preferences_dialog(); return 1;
      case FL_Page_Up: move_tab(-1); return 1;
      case FL_Page_Down: move_tab(1); return 1;
    }
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
  std::string workdir, title, ssh_arg, scheme_arg;
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
  g.area = new Fl_Group(0, 0, 800, 500);
  g.area->box(FL_NO_BOX);
  g.area->end();
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
  g.area->begin();
  // the first terminal is created before the window is shown so the window
  // gets the size for cols x rows
  TermWidget *t = new TermWidget(0, 0, 800, 500);
  g.area->end();
  if (g.set.font_size > 0) t->set_font_size(g.set.font_size);
  t->set_colors(*scheme_colors(scheme_override.empty() ? g.set.scheme : scheme_override));
  t->set_opacity(g.set.opacity);
  t->set_scrollback_limit((size_t)g.set.scrollback);
  t->set_hold(hold);
  t->set_hold_on_error(hold_on_error);
  t->on_title = on_title;
  t->on_exit = on_exit;
  t->on_menu = show_menu;
  g.tabs.push_back({t, !scheme_override.empty(), first, ""});
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
