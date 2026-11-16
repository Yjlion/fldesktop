// fl-term: terminal emulator for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_ask.H>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "term_widget.h"

static const char *kUsage =
    "Usage: fl-term [options] [-e command [args...]]\n"
    "  -e, --command CMD...    run CMD instead of the shell (rest of the arguments)\n"
    "  -d, --workdir DIR       start in DIR\n"
    "  -T, --title TITLE       window title\n"
    "  -g, --geometry COLSxROWS  initial size (default 90x26)\n"
    "      --hold              keep the window open after the command exits\n"
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

struct App {
  Fl_Double_Window *win = nullptr;
  TermWidget *term = nullptr;
  std::string fixed_title;
  std::vector<std::string> self_argv0;
};

static App g;

static void on_title(TermWidget *, const char *t, void *) {
  if (!g.fixed_title.empty()) return;
  g.win->copy_label(*t ? t : "Terminal");
}

static void on_exit(TermWidget *, int, void *) {
  g.win->hide();
}

static void new_window() {
  std::string dir = g.term->current_dir();
  std::vector<std::string> argv{"fl-term"};
  if (!dir.empty()) { argv.push_back("--workdir"); argv.push_back(dir); }
  fld::spawn(argv);
}

static void menu_cb(Fl_Widget *, void *v) {
  long id = (long)v;
  switch (id) {
    case 1: g.term->copy_selection(1); break;
    case 2: g.term->paste(1); break;
    case 3: new_window(); break;
    case 4: g.term->set_font_size(g.term->font_size() + 1); break;
    case 5: g.term->set_font_size(g.term->font_size() - 1); break;
    case 6: g.term->set_font_size(fld::mono_size()); break;
    case 7: {
      std::string d = g.term->current_dir();
      fld::spawn({"fl-files", d.empty() ? fld::home_dir() : d});
      break;
    }
    case 8: fld::about_dialog("Terminal", "utilities-terminal",
                              "A terminal emulator built with FLTK and libvterm."); break;
    case 9: g.win->hide(); break;
  }
}

static void show_menu(TermWidget *, void *) {
  static Fl_Menu_Item items[] = {
      {"Copy", FL_CTRL + FL_SHIFT + 'c', menu_cb, (void *)1},
      {"Paste", FL_CTRL + FL_SHIFT + 'v', menu_cb, (void *)2, FL_MENU_DIVIDER},
      {"New Window", FL_CTRL + FL_SHIFT + 'n', menu_cb, (void *)3},
      {"Open Folder", 0, menu_cb, (void *)7, FL_MENU_DIVIDER},
      {"Zoom In", FL_CTRL + '=', menu_cb, (void *)4},
      {"Zoom Out", FL_CTRL + '-', menu_cb, (void *)5},
      {"Normal Size", FL_CTRL + '0', menu_cb, (void *)6, FL_MENU_DIVIDER},
      {"About", 0, menu_cb, (void *)8},
      {"Close", FL_CTRL + FL_SHIFT + 'w', menu_cb, (void *)9},
      {nullptr}};
  const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y());
  if (m && m->callback()) m->do_callback(g.win);
}

static int global_shortcuts(int e) {
  if (e != FL_SHORTCUT && e != FL_KEYBOARD) return 0;
  int st = Fl::event_state();
  if ((st & (FL_CTRL | FL_SHIFT)) == (FL_CTRL | FL_SHIFT)) {
    switch (Fl::event_key()) {
      case 'n': new_window(); return 1;
      case 'w': g.win->hide(); return 1;
    }
  }
  return 0;
}

static void close_cb(Fl_Widget *w, void *) { w->hide(); }

int main(int argc, char **argv) {
  fld::init_app("fl-term", argc, argv);
  std::vector<std::string> cmd;
  std::string workdir, title;
  int cols = 90, rows = 26;
  bool hold = false;
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

  fld::Ini cfg;
  cfg.load(fld::fld_config_dir() + "/term.ini");
  std::string scheme = cfg.get("Terminal", "ColorScheme", "dark");
  const TermColors *colors = &kSchemes[0].c;
  for (auto &s : kSchemes)
    if (scheme == s.name) colors = &s.c;

  const int SBW = 0;
  g.win = new Fl_Double_Window(800, 500, title.empty() ? "Terminal" : title.c_str());
  g.fixed_title = title;
  g.term = new TermWidget(0, 0, 800 - SBW, 500);
  int fs = cfg.get_int("Terminal", "FontSize", 0);
  if (fs > 0) g.term->set_font_size(fs);
  g.term->set_colors(*colors);
  g.term->set_scrollback_limit((size_t)cfg.get_int("Terminal", "Scrollback", 10000));
  g.term->set_hold(hold);
  g.term->on_title = on_title;
  g.term->on_exit = on_exit;
  g.term->on_menu = show_menu;
  g.win->resizable(g.term);
  g.win->callback(close_cb);
  g.win->end();

  int ww, wh;
  g.term->size_for(cols, rows, ww, wh);
  g.win->size(ww + SBW, wh);
  g.win->size_range(g.term->cell_w() * 10 + SBW, g.term->cell_h() * 3, 0, 0, g.term->cell_w(),
                    g.term->cell_h());
  fld::set_window_icon(g.win, "utilities-terminal");
  Fl::add_handler(global_shortcuts);

  g.win->show();
  if (!g.term->start(cmd, workdir.empty() ? "" : fld::expand_tilde(workdir))) {
    fl_alert("Could not start the terminal process.");
    return 1;
  }
  g.term->take_focus();
  return Fl::run();
}
