#include "fld/window_size.h"

#include <FL/Fl.H>
#include <FL/Fl_Window.H>

#include <algorithm>
#include <vector>

#include "fld/ini.h"
#include "fld/util.h"

namespace fld {

namespace {

std::string state_file() {
  std::string dir = getenv_or("XDG_STATE_HOME", home_dir() + "/.local/state") + "/fldesktop";
  mkdir_p(dir);
  return dir + "/windows.ini";
}

struct Tracked {
  Fl_Window *win;
  std::string app;
  int w, h; // as last saved
};

std::vector<Tracked> &tracked() {
  static std::vector<Tracked> t;
  return t;
}

void save(void *) {
  Ini ini;
  ini.load(state_file());
  bool any = false;
  for (Tracked &t : tracked()) {
    if (!t.win->shown() || (t.win->w() == t.w && t.win->h() == t.h)) continue;
    // a maximised or full screen window: its own size stays
    if (t.win->maximize_active() || t.win->fullscreen_active()) continue;
    t.w = t.win->w();
    t.h = t.win->h();
    ini.set(t.app, "Width", std::to_string(t.w));
    ini.set(t.app, "Height", std::to_string(t.h));
    any = true;
  }
  if (any) ini.save(state_file());
}

// after each round of events: a window whose size changed is saved a moment
// after the resizing stops
void check(void *) {
  for (Tracked &t : tracked())
    if (t.win->shown() && (t.win->w() != t.w || t.win->h() != t.h)) {
      Fl::remove_timeout(save);
      Fl::add_timeout(1.0, save);
      return;
    }
}

} // namespace

void window_open_size(int saved_w, int saved_h, int def_w, int def_h, int area_w, int area_h, int min_w,
                      int min_h, int &w, int &h) {
  if (saved_w > 0 && saved_h > 0) {
    w = saved_w;
    h = saved_h;
  } else {
    w = def_w;
    h = std::min(def_h, area_h * 85 / 100);
  }
  w = std::max(min_w, std::min(w, area_w));
  h = std::max(min_h, std::min(h, area_h));
}

void remember_window_size(Fl_Window *win, const std::string &app) {
  Ini ini;
  ini.load(state_file());
  int X, Y, W, H;
  Fl::screen_work_area(X, Y, W, H);
  int w, h;
  window_open_size(ini.get_int(app, "Width", 0), ini.get_int(app, "Height", 0), win->w(), win->h(), W, H,
                   std::min(win->w(), 200), std::min(win->h(), 150), w, h);
  if (w != win->w() || h != win->h()) win->size(w, h);
  bool first = tracked().empty();
  tracked().push_back({win, app, w, h});
  if (first) Fl::add_check(check);
}

} // namespace fld
