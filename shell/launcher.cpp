// fl-launcher: application menu / run dialog
//
// app_id "fldesktop.launcher": fl-wm places it next to the panel's menu button
// and closes it when another window gets the focus. Starting it while it is
// already open closes the running instance (so the panel button toggles).
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>
#include <vector>

#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

static std::vector<fld::DesktopEntry> g_apps;
static std::vector<int> g_visible;         // indexes into g_apps
static std::string g_category = "All";
static std::vector<std::string> g_recent;  // desktop ids, most recent first
static Fl_Double_Window *g_win;

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
      fl_draw(fld::escape_at(e.name).c_str(), x() + 54, ry + 4, w() - 64, 20, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      std::string sub = !e.comment.empty() ? e.comment : e.generic_name;
      fl_font(FL_HELVETICA, 12);
      fl_color(i == sel ? fl_color_average(p.accent_fg, p.accent, 0.8f) : p.fg_dim);
      fl_draw(fld::escape_at(sub).c_str(), x() + 54, ry + 23, w() - 64, 18, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
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

int AppList::handle(int e) {
  switch (e) {
    case FL_PUSH: {
      int i = top + (Fl::event_y() - y()) / row_h;
      if (i < (int)g_visible.size()) {
        sel = i;
        redraw();
        if (Fl::event_button() == FL_LEFT_MOUSE) launch_index(i);
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
  CategoryList(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
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

class SearchInput : public Fl_Input {
public:
  using Fl_Input::Fl_Input;
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

static std::string pidfile() {
  return fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-launcher-" + std::to_string(getuid()) + ".pid";
}

static bool toggle_existing() {
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

static void on_term(int) { _exit(0); }

// The launcher closes when another window gets the keyboard focus. Clicking the
// panel's Apps button to close it would otherwise immediately start a new one,
// so the time of such a close is remembered for a moment.
static std::string closed_stamp() {
  return fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-launcher-" + std::to_string(getuid()) + ".closed";
}

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

static void focus_check(void *) {
  static bool had_focus = false;
  if (Fl::focus()) had_focus = true;
  else if (had_focus && g_win->shown()) {
    char buf[64];
    snprintf(buf, sizeof buf, "%.3f", now_s());
    fld::write_file(closed_stamp(), buf);
    quit_launcher();
  }
}

int main(int argc, char **argv) {
  if (toggle_existing() || just_closed()) return 0;
  fld::write_file(pidfile(), std::to_string(getpid()));
  signal(SIGTERM, on_term);

  fld::init_app("fldesktop.launcher", argc, argv);
  g_apps = fld::all_applications();
  load_recent();

  const int W = 640, H = 520, SIDE = 190;
  g_win = new Fl_Double_Window(W, H, "fldesktop.launcher");
  g_win->border(0);
  g_win->color(fld::palette().border);
  g_win->box(FL_BORDER_BOX);

  Fl_Box *bg = new Fl_Box(1, 1, W - 2, 52);
  bg->box(FL_FLAT_BOX);
  bg->color(fld::palette().bg);
  g_search = new SearchInput(12, 12, W - 24, 32);
  g_search->textsize(15);
  g_search->box(FL_ROUNDED_BOX);
  g_search->color(fld::palette().bg2);
  g_search->when(FL_WHEN_CHANGED);
  g_search->tooltip("Type to search, Enter to start, Esc to close");

  g_cats = new CategoryList(1, 53, SIDE, H - 54);
  g_list = new AppList(SIDE + 1, 53, W - SIDE - 14, H - 54);
  Fl_Scrollbar *sb = new Fl_Scrollbar(W - 13, 53, 12, H - 54);
  sb->type(FL_VERTICAL);
  sb->callback([](Fl_Widget *w, void *) {
    g_list->top = ((Fl_Scrollbar *)w)->value();
    g_list->redraw();
  });
  sb->clear_visible_focus();
  g_list->sb = sb;
  g_win->end();

  // "All" starts with recently used applications first
  std::vector<fld::DesktopEntry> ordered;
  for (auto &id : g_recent)
    for (auto &a : g_apps) if (a.id == id) ordered.push_back(a);
  for (auto &a : g_apps)
    if (std::find(g_recent.begin(), g_recent.end(), a.id) == g_recent.end()) ordered.push_back(a);
  g_apps = ordered;

  refilter();
  g_win->show(argc, argv);
  g_search->take_focus();
  Fl::add_check(focus_check);
  int r = Fl::run();
  unlink(pidfile().c_str());
  return r;
}
