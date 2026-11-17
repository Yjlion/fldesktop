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
#include "ssh_ui.h"
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
    case 20: ssh_ui::connect_dialog(); break;
    case 21: ssh_ui::manage_dialog(); break;
    case 22: ssh_ui::copy_key_dialog(); break;
    case 23: ssh_ui::generate_key(); break;
    case 24: ssh_ui::browse_files_dialog(); break;
  }
}

static std::vector<ssh::Profile> g_menu_profiles;
static std::vector<std::string> g_menu_mounts;

static void ssh_profile_cb(Fl_Widget *, void *v) {
  long i = (long)v;
  if (i >= 0 && i < (long)g_menu_profiles.size()) ssh_ui::connect(g_menu_profiles[(size_t)i]);
}

static void unmount_cb(Fl_Widget *, void *v) {
  long i = (long)v;
  if (i >= 0 && i < (long)g_menu_mounts.size()) ssh_ui::unmount(g_menu_mounts[(size_t)i]);
}

static void show_menu(TermWidget *, void *) {
  // labels must outlive the popup
  static std::vector<std::string> labels;
  labels.clear();
  g_menu_profiles = ssh_ui::profiles();
  g_menu_mounts = ssh_ui::sshfs_mounts();
  auto esc = [](const std::string &t) {
    std::string e;
    for (char c : t) {
      if (c == '/' || c == '\\' || c == '&' || c == '_' || c == '@') e += '\\';
      e += c;
    }
    return e;
  };
  labels.reserve(g_menu_profiles.size() + g_menu_mounts.size());
  std::vector<Fl_Menu_Item> m = {
      {"Copy", FL_CTRL + FL_SHIFT + 'c', menu_cb, (void *)1},
      {"Paste", FL_CTRL + FL_SHIFT + 'v', menu_cb, (void *)2, FL_MENU_DIVIDER},
      {"New Window", FL_CTRL + FL_SHIFT + 'n', menu_cb, (void *)3},
      {"Open Folder", 0, menu_cb, (void *)7, FL_MENU_DIVIDER},
      {"SSH", 0, nullptr, nullptr, FL_SUBMENU},
      {"Connect…", FL_CTRL + FL_SHIFT + 's', menu_cb, (void *)20, FL_MENU_DIVIDER},
  };
  for (size_t i = 0; i < g_menu_profiles.size() && i < 30; i++) {
    labels.push_back(esc(g_menu_profiles[i].name));
    m.push_back({labels.back().c_str(), 0, ssh_profile_cb, (void *)(long)i});
  }
  if (!g_menu_profiles.empty()) m.back().flags |= FL_MENU_DIVIDER;
  m.push_back({"Manage Connections…", 0, menu_cb, (void *)21});
  m.push_back({"Copy Public Key to Host…", 0, menu_cb, (void *)22});
  m.push_back({"Create SSH Key…", 0, menu_cb, (void *)23});
  if (ssh_ui::have_sshfs()) {
    m.back().flags |= FL_MENU_DIVIDER;
    m.push_back({"Browse Files (sshfs)…", 0, menu_cb, (void *)24});
    if (!g_menu_mounts.empty()) {
      m.push_back({"Unmount", 0, nullptr, nullptr, FL_SUBMENU});
      for (size_t i = 0; i < g_menu_mounts.size(); i++) {
        labels.push_back(esc(fld::basename(g_menu_mounts[i])));
        m.push_back({labels.back().c_str(), 0, unmount_cb, (void *)(long)i});
      }
      m.push_back({nullptr});
    }
  }
  m.push_back({nullptr}); // end of the SSH submenu
  std::vector<Fl_Menu_Item> rest = {
      {"Zoom In", FL_CTRL + '=', menu_cb, (void *)4},
      {"Zoom Out", FL_CTRL + '-', menu_cb, (void *)5},
      {"Normal Size", FL_CTRL + '0', menu_cb, (void *)6, FL_MENU_DIVIDER},
      {"About", 0, menu_cb, (void *)8},
      {"Close", FL_CTRL + FL_SHIFT + 'w', menu_cb, (void *)9},
      {nullptr}};
  m.insert(m.end(), rest.begin(), rest.end());
  // a divider after the SSH submenu
  for (auto &it : m)
    if (it.label() && std::string(it.label()) == "SSH") it.flags |= FL_MENU_DIVIDER;
  const Fl_Menu_Item *r = m[0].popup(Fl::event_x(), Fl::event_y());
  if (r && r->callback()) r->do_callback(g.win);
}

static int global_shortcuts(int e) {
  if (e != FL_SHORTCUT && e != FL_KEYBOARD) return 0;
  int st = Fl::event_state();
  if ((st & (FL_CTRL | FL_SHIFT)) == (FL_CTRL | FL_SHIFT)) {
    switch (Fl::event_key()) {
      case 'n': new_window(); return 1;
      case 'w': g.win->hide(); return 1;
      case 's': ssh_ui::connect_dialog(); return 1;
    }
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

  fld::Ini cfg;
  cfg.load(fld::fld_config_dir() + "/term.ini");
  std::string scheme = cfg.get("Terminal", "ColorScheme", "dark");
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
  if (!scheme_arg.empty()) scheme = scheme_arg;
  const TermColors *colors = find_scheme(scheme);
  if (!colors) colors = &kSchemes[0].c;

  const int SBW = 0;
  g.win = new Fl_Double_Window(800, 500, title.empty() ? "Terminal" : title.c_str());
  g.fixed_title = title;
  g.term = new TermWidget(0, 0, 800 - SBW, 500);
  int fs = cfg.get_int("Terminal", "FontSize", 0);
  if (fs > 0) g.term->set_font_size(fs);
  g.term->set_colors(*colors);
  g.term->set_scrollback_limit((size_t)cfg.get_int("Terminal", "Scrollback", 10000));
  g.term->set_hold(hold);
  g.term->set_hold_on_error(hold_on_error);
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
