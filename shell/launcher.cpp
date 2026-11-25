// fl-launcher: application menu / run dialog
//
// app_id "fldesktop.launcher": fl-wm places it next to the panel's menu button
// and closes it when another window gets the focus. Starting it while it is
// already open closes the running instance (so the panel button toggles).
//
// `fl-launcher --daemon` (started with the session) builds the window once and
// keeps it hidden; SIGUSR1 (fld::toggle_launcher, fl-wm's Super+Space) shows
// or hides it, so it opens in one frame instead of a program start.
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <memory>
#include <sys/inotify.h>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "fld/config_watch.h"
#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/session.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fld/text.h"

static std::vector<fld::DesktopEntry> g_apps;
static std::vector<int> g_visible;         // indexes into g_apps
static std::string g_category = "All";
static std::vector<std::string> g_recent;  // desktop ids, most recent first
static Fl_Double_Window *g_win;
static int g_modal = 0; // a menu or dialog is open: focus_check waits

static const char *kCategories[][2] = {
    {"All", "applications-other"},          {"AudioVideo", "applications-multimedia"},
    {"Development", "applications-development"}, {"Education", "applications-science"},
    {"Game", "applications-games"},         {"Graphics", "applications-graphics"},
    {"Network", "applications-internet"},   {"Office", "applications-office"},
    {"Settings", "preferences-desktop"},    {"System", "applications-system"},
    {"Utility", "applications-utilities"},  {"Other", "applications-other"},
};

static const char *category_label(const std::string &c) {
  if (c == "AudioVideo") return "Multimedia";
  if (c == "Game") return "Games";
  if (c == "Network") return "Internet";
  if (c == "Utility") return "Accessories";
  if (c == "All") return "All Applications";
  return c.c_str();
}

static std::string recent_path() { return fld::fld_config_dir() + "/launcher-recent"; }

static void load_recent() {
  std::string s;
  if (fld::read_file(recent_path(), s)) g_recent = fld::split(s, '\n');
}

static void push_recent(const std::string &id) {
  g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), id), g_recent.end());
  g_recent.insert(g_recent.begin(), id);
  if (g_recent.size() > 20) g_recent.resize(20);
  fld::write_file(recent_path(), fld::join(g_recent, "\n"));
}

static int match_score(const fld::DesktopEntry &e, const std::string &q) {
  if (q.empty()) return 1;
  std::string n = fld::lower(e.name);
  if (fld::starts_with(n, q)) return 100;
  for (auto &w : fld::split(n, ' ')) if (fld::starts_with(w, q)) return 80;
  if (n.find(q) != std::string::npos) return 60;
  if (fld::lower(e.generic_name).find(q) != std::string::npos) return 40;
  for (auto &k : e.keywords) if (fld::lower(k).find(q) != std::string::npos) return 30;
  if (fld::lower(e.exec).find(q) != std::string::npos) return 20;
  if (fld::lower(e.comment).find(q) != std::string::npos) return 10;
  return 0;
}

// ---- the application list ---------------------------------------------------

class AppList : public Fl_Widget {
public:
  int sel = 0, top = 0, row_h = 46;
  Fl_Scrollbar *sb = nullptr;
  AppList(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  int rows() const { return h() / row_h; }
  void ensure_visible() {
    if (sel < top) top = sel;
    if (sel >= top + rows()) top = sel - rows() + 1;
    if (top < 0) top = 0;
    update_sb();
    redraw();
  }
  void update_sb() {
    if (!sb) return;
    int n = (int)g_visible.size();
    sb->value(top, rows(), 0, std::max(n, rows()));
    if (n > rows()) sb->show(); else sb->hide();
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_push_clip(x(), y(), w(), h());
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    int n = (int)g_visible.size();
    if (n == 0) {
      fl_font(FL_HELVETICA, 14);
      fl_color(p.fg_dim);
      fl_draw("No applications found.\nPress Enter to run the text as a command.", x(), y(), w(), 80,
              FL_ALIGN_CENTER | FL_ALIGN_WRAP);
    }
    for (int i = top; i < n && (i - top) * row_h < h(); i++) {
      const fld::DesktopEntry &e = g_apps[g_visible[i]];
      int ry = y() + (i - top) * row_h;
      if (i == sel) {
        fl_color(p.accent);
        fl_rounded_rectf(x() + 4, ry + 2, w() - 8, row_h - 4, 6);
      }
      Fl_Image *img = fld::icon_first({e.icon, "application-x-executable"}, 32);
      if (img) img->draw(x() + 12, ry + (row_h - 32) / 2);
      Fl_Color fg = i == sel ? p.accent_fg : p.fg;
      fl_font(FL_HELVETICA_BOLD, 14);
      fl_color(fg);
      // cut with "…" (the whole text is in the search, Enter starts it)
      fl_draw(fld::escape_at(fld::ellipsize(e.name, w() - 64)).c_str(), x() + 54, ry + 4, w() - 64, 20,
              FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      std::string sub = !e.comment.empty() ? e.comment : e.generic_name;
      fl_font(FL_HELVETICA, 12);
      fl_color(i == sel ? fl_color_average(p.accent_fg, p.accent, 0.8f) : p.fg_dim);
      fl_draw(fld::escape_at(fld::ellipsize(sub, w() - 64)).c_str(), x() + 54, ry + 23, w() - 64, 18,
              FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    }
    fl_pop_clip();
  }
  int handle(int e) override;
};

static AppList *g_list;
static Fl_Input *g_search;

static void quit_launcher() { g_win->hide(); }

static void refilter() {
  std::string q = fld::lower(fld::trim(g_search->value()));
  std::vector<std::pair<int, int>> scored;
  for (int i = 0; i < (int)g_apps.size(); i++) {
    const auto &e = g_apps[i];
    if (q.empty() && g_category != "All" && e.main_category() != g_category) continue;
    int s = match_score(e, q);
    if (s > 0) scored.push_back({s, i});
  }
  if (!q.empty())
    std::stable_sort(scored.begin(), scored.end(), [](auto &a, auto &b) { return a.first > b.first; });
  g_visible.clear();
  for (auto &s : scored) g_visible.push_back(s.second);
  g_list->sel = 0;
  g_list->top = 0;
  g_list->update_sb();
  g_list->redraw();
}

static void launch_index(int vis_index) {
  if (vis_index < 0 || vis_index >= (int)g_visible.size()) return;
  const auto &e = g_apps[g_visible[vis_index]];
  e.launch();
  push_recent(e.id);
  quit_launcher();
}

static void run_text() {
  std::string t = fld::trim(g_search->value());
  if (t.empty()) return;
  fld::spawn_shell(t);
  quit_launcher();
}

// ---- the right-click menu of an application ----------------------------------------------

static std::string bare_id(const std::string &id) {
  return fld::ends_with(id, ".desktop") ? id.substr(0, id.size() - 8) : id;
}

static std::string panel_ini() { return fld::fld_config_dir() + "/panel.ini"; }

static std::vector<std::string> quick_launch() {
  fld::Ini c;
  c.load(panel_ini());
  std::vector<std::string> r;
  for (auto &x : fld::split(c.get("Panel", "QuickLaunch", "fl-files;fl-term;fl-edit"), ';'))
    if (!fld::trim(x).empty()) r.push_back(bare_id(fld::trim(x)));
  return r;
}

// the panel restarts when panel.ini changes
static void set_quick_launch(const std::vector<std::string> &ids) {
  fld::Ini c;
  c.load(panel_ini());
  c.set("Panel", "QuickLaunch", fld::join(ids, ";"));
  c.save(panel_ini());
}

static void add_to_desktop(const fld::DesktopEntry &e) {
  std::string dir = fld::user_dir("DESKTOP");
  fld::mkdir_p(dir);
  std::string dst = fld::path_join(dir, fld::ends_with(e.id, ".desktop") ? e.id : e.id + ".desktop");
  std::string data;
  if (!fld::read_file(e.path, data) || !fld::write_file(dst, data)) {
    fl_alert(_("Could not copy the launcher to %s."), dir.c_str());
    return;
  }
  chmod(dst.c_str(), 0755); // desktop files on the desktop must be executable to be trusted
}

static void app_menu(int vis_index) {
  if (vis_index < 0 || vis_index >= (int)g_visible.size()) return;
  const fld::DesktopEntry e = g_apps[g_visible[vis_index]]; // a copy: the list may change
  std::string id = bare_id(e.id);
  auto ql = quick_launch();
  bool in_ql = std::find(ql.begin(), ql.end(), id) != ql.end();
  bool in_recent = std::find(g_recent.begin(), g_recent.end(), e.id) != g_recent.end();
  fld::Menu m;
  m.item("Open", [vis_index] { launch_index(vis_index); });
  for (auto &a : e.actions)
    m.item(fld::Menu::text(a.name), [e, a] {
      e.launch_action(a.id);
      push_recent(e.id);
      quit_launcher();
    });
  m.divider();
  if (in_ql)
    m.item("Remove from Quick Launch", [id] {
      auto l = quick_launch();
      l.erase(std::remove(l.begin(), l.end(), id), l.end());
      set_quick_launch(l);
    });
  else
    m.item("Add to Quick Launch", [id] {
      auto l = quick_launch();
      l.push_back(id);
      set_quick_launch(l);
    });
  m.item("Add to Desktop", [e] { add_to_desktop(e); });
  if (in_recent)
    m.item("Remove from Recent", [e] {
      g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), e.id), g_recent.end());
      fld::write_file(recent_path(), fld::join(g_recent, "\n"));
    });
  m.divider();
  m.item("Show Desktop File", [e] { fld::spawn({"fl-files", e.path}); });
  g_modal++;
  m.popup();
  g_modal--;
}

int AppList::handle(int e) {
  switch (e) {
    case FL_PUSH: {
      int i = top + (Fl::event_y() - y()) / row_h;
      if (i < (int)g_visible.size()) {
        sel = i;
        redraw();
        if (Fl::event_button() == FL_LEFT_MOUSE) launch_index(i);
        else if (Fl::event_button() == FL_RIGHT_MOUSE) app_menu(i);
      }
      return 1;
    }
    case FL_MOUSEWHEEL:
      top = std::max(0, std::min(top + Fl::event_dy() * 2, (int)g_visible.size() - rows()));
      update_sb();
      redraw();
      return 1;
    case FL_MOVE: {
      int i = top + (Fl::event_y() - y()) / row_h;
      if (i != sel && i < (int)g_visible.size()) {
        sel = i;
        redraw();
      }
      return 1;
    }
    case FL_ENTER:
      return 1;
  }
  return Fl_Widget::handle(e);
}

// ---- category sidebar ------------------------------------------------------------

class CategoryList : public Fl_Widget {
public:
  std::vector<std::string> cats;
  CategoryList(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { fill(); }
  // the categories that have applications
  void fill() {
    cats.clear();
    for (auto &c : kCategories) {
      std::string name = c[0];
      bool used = name == "All";
      for (auto &a : g_apps) if (a.main_category() == name) { used = true; break; }
      if (used) cats.push_back(name);
    }
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(p.bg);
    fl_rectf(x(), y(), w(), h());
    for (int i = 0; i < (int)cats.size(); i++) {
      int ry = y() + 4 + i * 34;
      bool on = cats[i] == g_category && !*g_search->value();
      if (on) {
        fl_color(fl_color_average(p.accent, p.bg, 0.25f));
        fl_rounded_rectf(x() + 4, ry, w() - 8, 32, 6);
      }
      const char *icon = "applications-other";
      for (auto &c : kCategories) if (cats[i] == c[0]) icon = c[1];
      if (Fl_Image *img = fld::icon(icon, 20)) img->draw(x() + 12, ry + 6);
      fl_font(on ? FL_HELVETICA_BOLD : FL_HELVETICA, 13);
      fl_color(p.fg);
      fl_draw(category_label(cats[i]), x() + 40, ry, w() - 44, 32, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    }
  }
  int handle(int e) override {
    if (e == FL_PUSH || (e == FL_DRAG && Fl::event_inside(this))) {
      int i = (Fl::event_y() - y() - 4) / 34;
      if (i >= 0 && i < (int)cats.size()) {
        g_category = cats[i];
        g_search->value("");
        refilter();
        redraw();
      }
      return 1;
    }
    return Fl_Widget::handle(e);
  }
};

static CategoryList *g_cats;

class SearchInput : public fld::SearchInput {
public:
  using fld::SearchInput::SearchInput;
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key();
      int n = (int)g_visible.size();
      switch (k) {
        case FL_Escape: quit_launcher(); return 1;
        case FL_Down: if (g_list->sel < n - 1) g_list->sel++; g_list->ensure_visible(); return 1;
        case FL_Up: if (g_list->sel > 0) g_list->sel--; g_list->ensure_visible(); return 1;
        case FL_Page_Down: g_list->sel = std::min(n - 1, g_list->sel + g_list->rows()); g_list->ensure_visible(); return 1;
        case FL_Page_Up: g_list->sel = std::max(0, g_list->sel - g_list->rows()); g_list->ensure_visible(); return 1;
        case FL_Enter: case FL_KP_Enter:
          if (n > 0) launch_index(g_list->sel);
          else run_text();
          return 1;
      }
      int r = Fl_Input::handle(e);
      refilter();
      g_cats->redraw();
      return r;
    }
    return Fl_Input::handle(e);
  }
};

// ---- single instance toggle ------------------------------------------------------

static std::string pidfile() { return fld::launcher_pidfile(); }

static bool g_daemon = false;

// another fl-launcher is open: close it (a resident one is asked to toggle)
static bool toggle_existing() {
  if (int pid = fld::resident_launcher()) return kill(pid, SIGUSR1) == 0;
  std::string s;
  if (!fld::read_file(pidfile(), s)) return false;
  pid_t pid = atoi(s.c_str());
  if (pid <= 1 || pid == getpid()) return false;
  // make sure it really is a launcher
  std::string comm;
  fld::read_file("/proc/" + std::to_string(pid) + "/comm", comm);
  if (fld::trim(comm) != "fl-launcher") return false;
  return kill(pid, SIGTERM) == 0;
}

static void on_term(int) {
  if (g_daemon) unlink(pidfile().c_str());
  _exit(0);
}

// The launcher closes when another window gets the keyboard focus. Clicking the
// panel's Apps button to close it would otherwise immediately start a new one,
// so the time of such a close is remembered for a moment.
static std::string closed_stamp() { return fld::launcher_closed_stamp(); }

static double now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

static bool just_closed() {
  std::string s;
  if (!fld::read_file(closed_stamp(), s)) return false;
  unlink(closed_stamp().c_str());
  return now_s() - atof(s.c_str()) < 0.6;
}

static bool g_had_focus = false;

static void focus_check(void *) {
  if (g_modal) return;
  if (Fl::focus()) g_had_focus = true;
  else if (g_had_focus && g_win->shown()) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.3f", now_s());
    fld::write_file(closed_stamp(), buf);
    quit_launcher();
  }
}

// ---- the bottom bar: settings and power ------------------------------------------------------

static std::vector<std::pair<fld::PowerAction, Fl_Widget *>> g_power_buttons;

// asks logind which power actions are possible: after the window is up (the
// three D-Bus calls would hold up the first frame), or once for the resident one
static void check_power(void *) {
  for (auto &pb : g_power_buttons) {
    if (fld::can_power(pb.first)) pb.second->activate();
    else pb.second->deactivate();
  }
}

static void make_power_bar(int X, int Y, int W, int H) {
  const fld::Palette &p = fld::palette();
  Fl_Box *bg = new Fl_Box(X, Y, W, H);
  bg->box(FL_FLAT_BOX);
  bg->color(p.bg);
  Fl_Box *line = new Fl_Box(X, Y, W, 1);
  line->box(FL_FLAT_BOX);
  line->color(p.border);
  const int B = 36, pad = (H - B) / 2;
  fld::FlatButton *set = new fld::FlatButton(X + 8, Y + pad, 120, B, _("Settings"));
  set->icon_name("preferences-system", 20);
  set->color(p.bg);
  set->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_IMAGE_NEXT_TO_TEXT);
  set->tooltip(_("Open Settings"));
  set->callback([](Fl_Widget *, void *) {
    fld::spawn({"fl-settings"});
    quit_launcher();
  });
  using fld::PowerAction;
  static const PowerAction acts[] = {PowerAction::Lock, PowerAction::LogOut, PowerAction::Suspend,
                                     PowerAction::Reboot, PowerAction::PowerOff};
  int bx = X + W - 8 - 5 * (B + 4);
  for (PowerAction a : acts) {
    fld::FlatButton *b = new fld::FlatButton(bx, Y + pad, B, B);
    b->icon_name(fld::power_icon(a), 20);
    b->color(p.bg);
    b->tooltip(fld::power_label(a));
    g_power_buttons.push_back({a, b});
    b->callback([](Fl_Widget *, void *d) {
      PowerAction a = (PowerAction)(long)d;
      g_modal++;
      bool done = fld::confirm_power(a);
      g_modal--;
      if (done) quit_launcher();
      else if (g_win->shown()) g_search->take_focus();
    }, (void *)(long)a);
    bx += B + 4;
  }
}

// "All" starts with recently used applications first
static void order_by_recent() {
  std::vector<fld::DesktopEntry> ordered;
  for (auto &id : g_recent)
    for (auto &a : g_apps) if (a.id == id) ordered.push_back(a);
  for (auto &a : g_apps)
    if (std::find(g_recent.begin(), g_recent.end(), a.id) == g_recent.end()) ordered.push_back(a);
  g_apps = ordered;
}

// ---- the resident launcher ----------------------------------------------------------

static int g_wake[2] = {-1, -1};
static bool g_apps_changed = false;
static bool g_restart = false; // the appearance changed: start afresh when hidden
static double g_hidden_at = 0;
static int g_argc;
static char **g_argv;

static void on_usr1(int) {
  char c = 1;
  if (write(g_wake[1], &c, 1) < 0) {}
}

static void present() {
  if (g_apps_changed) {
    g_apps_changed = false;
    g_apps = fld::all_applications();
    g_cats->fill();
  }
  load_recent();
  order_by_recent();
  g_search->value("");
  g_category = "All";
  g_cats->redraw();
  refilter();
  g_had_focus = false;
  g_win->show();
  g_search->take_focus();
}

static void wake_cb(int fd, void *) {
  char buf[64];
  while (read(fd, buf, sizeof buf) > 0) {}
  if (g_win->shown()) {
    quit_launcher();
    return;
  }
  // closed a moment ago by the click (on the panel's button) that asks again
  if (now_s() - g_hidden_at < 0.6 && just_closed()) return;
  unlink(closed_stamp().c_str());
  present();
}

int main(int argc, char **argv) {
  g_argc = argc;
  g_argv = argv;
  for (int i = 1; i < argc; i++)
    if (!strcmp(argv[i], "--daemon")) g_daemon = true;
  if (g_daemon) {
    if (fld::resident_launcher()) return 0; // one is enough
  } else if (just_closed() || toggle_existing()) {
    return 0;
  }
  fld::write_file(pidfile(), std::to_string(getpid()) + (g_daemon ? " daemon" : ""));
  signal(SIGTERM, on_term);

  fld::init_app("fldesktop.launcher", argc, argv);
  // The shell window is borderless and fl-wm knows it by its title; any other
  // window (dialogs, fl_choice) gets an ordinary app id, or fl-wm would give
  // it the shell role too.
  Fl_Window::default_xclass("fl-launcher");
  g_apps = fld::all_applications();
  load_recent();

  const int W = 640, H = 568, SIDE = 190, BAR = 48;
  g_win = new Fl_Double_Window(W, H, "fldesktop.launcher");
  g_win->border(0);
  g_win->color(fld::palette().border);
  g_win->box(FL_BORDER_BOX);

  Fl_Box *bg = new Fl_Box(1, 1, W - 2, 52);
  bg->box(FL_FLAT_BOX);
  bg->color(fld::palette().bg);
  g_search = new SearchInput(12, 12, W - 24, 32, _("Search applications"));
  g_search->textsize(15);
  g_search->color(fld::palette().bg2);
  g_search->when(FL_WHEN_CHANGED);
  g_search->tooltip(_("Type to search, Enter to start, Esc to close"));

  g_cats = new CategoryList(1, 53, SIDE, H - 54 - BAR);
  g_list = new AppList(SIDE + 1, 53, W - SIDE - 14, H - 54 - BAR);
  Fl_Scrollbar *sb = new Fl_Scrollbar(W - 13, 53, 12, H - 54 - BAR);
  sb->type(FL_VERTICAL);
  sb->callback([](Fl_Widget *w, void *) {
    g_list->top = ((Fl_Scrollbar *)w)->value();
    g_list->redraw();
  });
  sb->clear_visible_focus();
  g_list->sb = sb;
  make_power_bar(1, H - BAR - 1, W - 2, BAR);
  g_win->end();
  order_by_recent();
  refilter();
  Fl::add_check(focus_check);

  if (!g_daemon) {
    g_win->show(); // not show(argc, argv): it resets the theme colors
    g_search->take_focus();
    Fl::add_timeout(0.05, check_power);
    int r = Fl::run();
    fld::wait_power(); // a refused restart still shows why
    unlink(pidfile().c_str());
    return r;
  }

  // resident: wait for SIGUSR1 with the window hidden
  check_power(nullptr);
  if (pipe2(g_wake, O_CLOEXEC | O_NONBLOCK) != 0) return 1;
  Fl::add_fd(g_wake[0], FL_READ, wake_cb);
  signal(SIGUSR1, on_usr1);
  // applications installed or removed: read them again before the next show
  // (also directories that appear later: the first Flatpak creates its
  // exports directory)
  std::vector<std::string> app_dirs;
  for (const std::string &d : fld::data_dirs()) app_dirs.push_back(d + "/applications");
  fld::DirWatcher apps_watch(app_dirs, [] { g_apps_changed = true; }, 0);
  // colours, fonts or the icon theme changed
  fld::ConfigWatcher appearance(fld::desktop_config_path(), [] { g_restart = true; });
  for (;;) {
    bool was_shown = g_win->shown();
    Fl::wait(1e20);
    if (was_shown && !g_win->shown()) {
      g_hidden_at = now_s();
      fld::wait_power();
    }
    if (g_restart && !g_win->shown()) {
      unlink(pidfile().c_str());
      fld::restart_self(g_argc, g_argv);
    }
  }
}
