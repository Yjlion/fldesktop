// fl-panel: the fldesktop taskbar
//
// A borderless FLTK window with app_id "fldesktop.panel"; fl-wm pins it to the
// bottom (or top) edge of the primary output and reserves its height. The
// task list comes from the wlr-foreign-toplevel-management protocol.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Slider.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <wayland-client.h>
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fld/config_watch.h"
#include "fld/menus.h"
#include "fld/session.h"
#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "panel.h"


// ---------------------------------------------------------------------------
// Task list model (foreign toplevels)

struct Task;
class TaskButton;

struct Task {
  zwlr_foreign_toplevel_handle_v1 *handle = nullptr;
  std::string title, app_id;
  bool activated = false, minimized = false, maximized = false, fullscreen = false;
  bool p_activated = false, p_minimized = false, p_maximized = false, p_fullscreen = false;
  TaskButton *button = nullptr;
  uint64_t order = 0;
};

static std::vector<std::unique_ptr<Task>> g_tasks;
static uint64_t g_task_counter = 0;
static wl_seat *g_seat = nullptr;
static zwlr_foreign_toplevel_manager_v1 *g_ftl_mgr = nullptr;
static void relayout_tasks();
static void update_task_button(Task *t);

static void ftl_title(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *title) {
  static_cast<Task *>(data)->title = title ? title : "";
}
static void ftl_app_id(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *app_id) {
  static_cast<Task *>(data)->app_id = app_id ? app_id : "";
}
static void ftl_output_enter(void *, zwlr_foreign_toplevel_handle_v1 *, wl_output *) {}
static void ftl_output_leave(void *, zwlr_foreign_toplevel_handle_v1 *, wl_output *) {}
static void ftl_state(void *data, zwlr_foreign_toplevel_handle_v1 *, wl_array *state) {
  Task *t = static_cast<Task *>(data);
  t->p_activated = t->p_minimized = t->p_maximized = t->p_fullscreen = false;
  uint32_t *s;
  for (s = (uint32_t *)state->data; (char *)s < (char *)state->data + state->size; s++) {
    switch (*s) {
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED: t->p_activated = true; break;
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED: t->p_minimized = true; break;
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED: t->p_maximized = true; break;
      case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN: t->p_fullscreen = true; break;
    }
  }
}
static void ftl_done(void *data, zwlr_foreign_toplevel_handle_v1 *) {
  Task *t = static_cast<Task *>(data);
  t->activated = t->p_activated;
  t->minimized = t->p_minimized;
  t->maximized = t->p_maximized;
  t->fullscreen = t->p_fullscreen;
  if (t->activated) {
    for (auto &o : g_tasks) if (o.get() != t && o->activated) { o->activated = false; update_task_button(o.get()); }
  }
  if (!t->button) relayout_tasks();
  else update_task_button(t);
}
static void ftl_closed(void *data, zwlr_foreign_toplevel_handle_v1 *h) {
  Task *t = static_cast<Task *>(data);
  zwlr_foreign_toplevel_handle_v1_destroy(h);
  for (auto it = g_tasks.begin(); it != g_tasks.end(); ++it) {
    if (it->get() == t) {
      g_tasks.erase(it);
      break;
    }
  }
  relayout_tasks();
}
static void ftl_parent(void *, zwlr_foreign_toplevel_handle_v1 *, zwlr_foreign_toplevel_handle_v1 *) {}

static const zwlr_foreign_toplevel_handle_v1_listener kHandleListener = {
    ftl_title, ftl_app_id, ftl_output_enter, ftl_output_leave, ftl_state, ftl_done, ftl_closed, ftl_parent,
};

static void mgr_toplevel(void *, zwlr_foreign_toplevel_manager_v1 *, zwlr_foreign_toplevel_handle_v1 *h) {
  auto t = std::make_unique<Task>();
  t->handle = h;
  t->order = ++g_task_counter;
  zwlr_foreign_toplevel_handle_v1_add_listener(h, &kHandleListener, t.get());
  g_tasks.push_back(std::move(t));
}
static void mgr_finished(void *, zwlr_foreign_toplevel_manager_v1 *) { g_ftl_mgr = nullptr; }
static const zwlr_foreign_toplevel_manager_v1_listener kMgrListener = {mgr_toplevel, mgr_finished};

static void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
#ifdef FLD_HAVE_PAGER
  if (pager_bind(reg, name, iface, version)) return;
#endif
  if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
    g_ftl_mgr = (zwlr_foreign_toplevel_manager_v1 *)wl_registry_bind(
        reg, name, &zwlr_foreign_toplevel_manager_v1_interface, std::min<uint32_t>(version, 3));
    zwlr_foreign_toplevel_manager_v1_add_listener(g_ftl_mgr, &kMgrListener, nullptr);
  } else if (!strcmp(iface, wl_seat_interface.name) && !g_seat) {
    g_seat = (wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
  }
}
static void reg_global_remove(void *, wl_registry *, uint32_t) {}
static const wl_registry_listener kRegListener = {reg_global, reg_global_remove};

// ---------------------------------------------------------------------------
// Widgets

Fl_Color panel_bg() { return fld::palette().panel_bg; }
Fl_Color panel_fg() { return fld::palette().panel_fg; }
Fl_Color panel_hover() { return fl_color_average(panel_bg(), FL_WHITE, 0.85f); }

static Fl_Image *icon_for_app_id(const std::string &app_id, int size) {
  static std::map<std::string, std::string> cache;
  auto it = cache.find(app_id);
  std::string icon;
  if (it != cache.end()) icon = it->second;
  else {
    fld::DesktopEntry e;
    if (fld::find_application(app_id, e) || fld::find_application(fld::lower(app_id), e)) icon = e.icon;
    else {
      // reverse-DNS app ids: try the last component (org.gnome.Foo -> foo)
      size_t d = app_id.rfind('.');
      if (d != std::string::npos && fld::find_application(app_id.substr(d + 1), e)) icon = e.icon;
    }
    if (icon.empty()) icon = app_id;
    cache[app_id] = icon;
  }
  return fld::icon_first({icon, fld::lower(icon), "application-x-executable"}, size);
}

class TaskButton : public Fl_Widget {
public:
  Task *task;
  bool hover = false;
  TaskButton(Task *t, int x, int y, int w, int h) : Fl_Widget(x, y, w, h), task(t) {}
  void draw() override {
    Fl_Color bg = panel_bg();
    if (task->activated) bg = fl_color_average(panel_bg(), FL_WHITE, 0.78f);
    else if (hover) bg = panel_hover();
    fl_color(panel_bg());
    fl_rectf(x(), y(), w(), h());
    fl_color(bg);
    fl_rounded_rectf(x() + 1, y() + 3, w() - 2, h() - 6, 5);
    if (task->activated) {
      fl_color(fld::palette().accent);
      fl_rectf(x() + 8, y() + h() - 5, w() - 16, 2);
    }
    int ix = x() + 8;
    if (Fl_Image *img = icon_for_app_id(task->app_id, 20)) {
      img->draw(ix, y() + (h() - 20) / 2);
      ix += 26;
    }
    fl_font(FL_HELVETICA, 13);
    fl_color(task->minimized ? fl_color_average(panel_fg(), panel_bg(), 0.55f) : panel_fg());
    std::string t = task->title.empty() ? task->app_id : task->title;
    // ellipsize
    int avail = x() + w() - 8 - ix;
    if (fl_width(t.c_str()) > avail) {
      while (!t.empty() && fl_width((t + "…").c_str()) > avail) {
        t.pop_back();
        while (!t.empty() && (t.back() & 0xC0) == 0x80) t.pop_back();
      }
      t += "…";
    }
    fl_draw(fld::escape_at(t).c_str(), ix, y(), avail, h(), FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  }
  int handle(int e) override;
};

static void task_menu(Task *t) {
  Fl_Menu_Item items[] = {
      {t->maximized ? "Restore" : "Maximize", 0, nullptr, (void *)1},
      {t->minimized ? "Show" : "Minimize", 0, nullptr, (void *)2, FL_MENU_DIVIDER},
      {"Close", 0, nullptr, (void *)3},
      {nullptr}};
  const Fl_Menu_Item *m = items->popup(Fl::event_x(), Fl::event_y());
  if (!m) return;
  switch ((long)m->user_data()) {
    case 1:
      if (t->maximized) zwlr_foreign_toplevel_handle_v1_unset_maximized(t->handle);
      else zwlr_foreign_toplevel_handle_v1_set_maximized(t->handle);
      break;
    case 2:
      if (t->minimized) {
        zwlr_foreign_toplevel_handle_v1_unset_minimized(t->handle);
        if (g_seat) zwlr_foreign_toplevel_handle_v1_activate(t->handle, g_seat);
      } else zwlr_foreign_toplevel_handle_v1_set_minimized(t->handle);
      break;
    case 3: zwlr_foreign_toplevel_handle_v1_close(t->handle); break;
  }
}

int TaskButton::handle(int e) {
  switch (e) {
    case FL_ENTER: hover = true; redraw(); return 1;
    case FL_LEAVE: hover = false; redraw(); return 1;
    case FL_PUSH:
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        task_menu(task);
        return 1;
      }
      if (Fl::event_button() == FL_MIDDLE_MOUSE) {
        zwlr_foreign_toplevel_handle_v1_close(task->handle);
        return 1;
      }
      return 1;
    case FL_RELEASE:
      if (Fl::event_button() != FL_LEFT_MOUSE || !Fl::event_inside(this)) return 1;
      if (task->activated && !task->minimized) {
        zwlr_foreign_toplevel_handle_v1_set_minimized(task->handle);
      } else {
        if (task->minimized) zwlr_foreign_toplevel_handle_v1_unset_minimized(task->handle);
        if (g_seat) zwlr_foreign_toplevel_handle_v1_activate(task->handle, g_seat);
      }
      return 1;
  }
  return Fl_Widget::handle(e);
}

// Calendar popup ---------------------------------------------------------------

class Calendar : public Fl_Widget {
public:
  int year, month; // month 0-11
  Calendar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    year = tm.tm_year + 1900;
    month = tm.tm_mon;
  }
  void step(int d) {
    month += d;
    while (month < 0) { month += 12; year--; }
    while (month > 11) { month -= 12; year++; }
    redraw();
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    struct tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month;
    tm.tm_mday = 1;
    tm.tm_hour = 12;
    mktime(&tm);
    char head[64];
    strftime(head, sizeof head, "%B %Y", &tm);
    fl_font(FL_HELVETICA_BOLD, 15);
    fl_color(p.fg);
    fl_draw(head, x(), y() + 4, w(), 28, FL_ALIGN_CENTER);
    fl_font(FL_HELVETICA, 13);
    fl_draw("‹", x() + 6, y() + 4, 24, 28, FL_ALIGN_CENTER);
    fl_draw("›", x() + w() - 30, y() + 4, 24, 28, FL_ALIGN_CENTER);
    int cw = (w() - 16) / 7, top = y() + 40;
    const char *days[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    fl_color(p.fg_dim);
    for (int i = 0; i < 7; i++) fl_draw(days[i], x() + 8 + i * cw, top, cw, 20, FL_ALIGN_CENTER);
    int wday = (tm.tm_wday + 6) % 7; // monday first
    int mdays = 31;
    for (int d = 28; d <= 31; d++) {
      struct tm t2{};
      t2.tm_year = year - 1900;
      t2.tm_mon = month;
      t2.tm_mday = d;
      t2.tm_hour = 12;
      mktime(&t2);
      if (t2.tm_mon != month) { mdays = d - 1; break; }
    }
    time_t now = time(nullptr);
    struct tm today;
    localtime_r(&now, &today);
    for (int d = 1; d <= mdays; d++) {
      int idx = wday + d - 1;
      int cx = x() + 8 + (idx % 7) * cw, cy = top + 24 + (idx / 7) * 26;
      bool is_today = today.tm_year + 1900 == year && today.tm_mon == month && today.tm_mday == d;
      if (is_today) {
        fl_color(p.accent);
        fl_pie(cx + cw / 2 - 12, cy, 24, 24, 0, 360);
        fl_color(p.accent_fg);
      } else fl_color(p.fg);
      char b[8];
      snprintf(b, sizeof b, "%d", d);
      fl_draw(b, cx, cy, cw, 24, FL_ALIGN_CENTER);
    }
  }
  int handle(int e) override {
    if (e == FL_PUSH) {
      if (Fl::event_y() < y() + 34) {
        if (Fl::event_x() < x() + 40) step(-1);
        else if (Fl::event_x() > x() + w() - 40) step(1);
      }
      return 1;
    }
    if (e == FL_MOUSEWHEEL) {
      step(Fl::event_dy() > 0 ? 1 : -1);
      return 1;
    }
    return Fl_Widget::handle(e);
  }
};

// ---------------------------------------------------------------------------
// Panel window

struct Panel {
  Fl_Double_Window *win = nullptr;
  fld::FlatButton *menu_btn = nullptr;
  Fl_Group *quick = nullptr;
  Fl_Widget *pager = nullptr;
  Fl_Group *tasks = nullptr;
  fld::FlatButton *volume = nullptr;
  fld::FlatButton *keyboard = nullptr;
  fld::FlatButton *network = nullptr;
  fld::FlatButton *bluetooth = nullptr;
  fld::FlatButton *printer = nullptr;
  fld::FlatButton *clipboard = nullptr;
  fld::FlatButton *battery = nullptr;
  fld::FlatButton *bell = nullptr;
  fld::FlatButton *clock = nullptr;
  fld::FlatButton *power = nullptr;
  bool top = false;
  std::string clock_fmt = "%a %d %b  %H:%M";
  int volume_pct = -1;
  bool muted = false;
  std::string bat_path;
  std::vector<std::string> quick_apps;
};

static Panel P;


static void update_task_button(Task *t) {
  if (t->button) t->button->redraw();
}

static void relayout_tasks() {
  if (!P.tasks) return;
  // drop buttons of tasks that are gone
  for (int i = P.tasks->children() - 1; i >= 0; i--) {
    TaskButton *b = (TaskButton *)P.tasks->child(i);
    bool alive = false;
    for (auto &t : g_tasks) if (t.get() == b->task) alive = true;
    if (!alive) {
      P.tasks->remove(b);
      Fl::delete_widget(b);
    }
  }
  std::vector<Task *> list;
  for (auto &t : g_tasks) list.push_back(t.get());
  std::sort(list.begin(), list.end(), [](Task *a, Task *b) { return a->order < b->order; });
  int n = (int)list.size();
  if (n) {
    int bw = std::min(220, P.tasks->w() / n);
    int x = P.tasks->x();
    for (Task *t : list) {
      if (!t->button) {
        t->button = new TaskButton(t, x, P.tasks->y(), bw, P.tasks->h());
        P.tasks->add(t->button);
      }
      t->button->resize(x, P.tasks->y(), bw - 2, P.tasks->h());
      t->button->copy_tooltip(t->title.c_str());
      x += bw;
    }
  }
  // the task area has no box: redraw the window so a removed button's pixels go
  if (P.win) P.win->redraw();
  else P.tasks->redraw();
}

static void tick(void *);

static void update_clock() {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  char buf[64];
  strftime(buf, sizeof buf, P.clock_fmt.c_str(), &tm);
  if (!P.clock->label() || strcmp(P.clock->label(), buf)) {
    P.clock->copy_label(buf);
    char tip[128];
    strftime(tip, sizeof tip, "%A, %d %B %Y", &tm);
    P.clock->copy_tooltip(tip);
    relayout();
  }
}

static bool read_battery(int &pct, std::string &status) {
  if (P.bat_path.empty()) return false;
  std::string cap, st;
  if (!fld::read_file(P.bat_path + "/capacity", cap)) return false;
  fld::read_file(P.bat_path + "/status", st);
  pct = atoi(cap.c_str());
  status = fld::trim(st);
  return true;
}

static void update_battery() {
  int pct;
  std::string status;
  if (!read_battery(pct, status)) {
    P.battery->hide();
    return;
  }
  P.battery->show();
  bool charging = status == "Charging" || status == "Full";
  const char *level = pct > 80 ? "full" : pct > 40 ? "good" : pct > 15 ? "low" : "caution";
  std::string icon = std::string("battery-") + level + (charging ? "-charging" : "");
  P.battery->icon_name(icon.c_str(), 18);
  char buf[16];
  snprintf(buf, sizeof buf, "%d%%", pct);
  P.battery->copy_label(buf);
  P.battery->copy_tooltip((status + ", " + buf).c_str());
}

// Volume via wpctl (PipeWire) or pactl (PulseAudio) ---------------------------

static bool have_wpctl() { static int v = -1; if (v < 0) v = !fld::which("wpctl").empty(); return v; }
static bool have_pactl() { static int v = -1; if (v < 0) v = !fld::which("pactl").empty(); return v; }

#ifdef FLD_HAVE_AUDIO
#include "audiobackend.h"
static AudioBackend *g_audio = nullptr;

static void update_volume_widget();
static void audio_changed() {
  const Audio::Device *d = g_audio->defaultDevice(Audio::Sink);
  bool was_visible = P.volume->visible();
  if (d && g_audio->isConnected()) {
    P.volume_pct = (int)lround(Audio::level(d->volumes) * 100);
    P.muted = d->muted;
  } else {
    P.volume_pct = -1;
  }
  update_volume_widget();
  if (was_visible != (bool)P.volume->visible()) relayout();
}
#endif

static void query_volume() {
#ifdef FLD_HAVE_AUDIO
  if (g_audio) return; // updates arrive from the PipeWire backend
#endif
  std::string out;
  P.volume_pct = -1;
  if (have_wpctl() && fld::run_capture({"wpctl", "get-volume", "@DEFAULT_AUDIO_SINK@"}, out) == 0) {
    // "Volume: 0.45 [MUTED]"
    double v = 0;
    if (sscanf(out.c_str(), "Volume: %lf", &v) == 1) P.volume_pct = (int)lround(v * 100);
    P.muted = out.find("MUTED") != std::string::npos;
  } else if (have_pactl() && fld::run_capture({"pactl", "get-sink-volume", "@DEFAULT_SINK@"}, out) == 0) {
    size_t p = out.find('%');
    if (p != std::string::npos) {
      size_t s = out.rfind(' ', p);
      P.volume_pct = atoi(out.substr(s + 1, p - s - 1).c_str());
    }
    std::string m;
    fld::run_capture({"pactl", "get-sink-mute", "@DEFAULT_SINK@"}, m);
    P.muted = m.find("yes") != std::string::npos;
  }
}

static void update_volume_widget() {
  if (P.volume_pct < 0) {
    P.volume->hide();
    return;
  }
  P.volume->show();
  const char *icon = P.muted || P.volume_pct == 0 ? "audio-volume-muted"
                     : P.volume_pct < 34          ? "audio-volume-low"
                     : P.volume_pct < 67          ? "audio-volume-medium"
                                                  : "audio-volume-high";
  P.volume->icon_name(icon, 18);
  char tip[64];
  snprintf(tip, sizeof tip, "Volume %d%%%s", P.volume_pct, P.muted ? " (muted)" : "");
  P.volume->copy_tooltip(tip);
  P.volume->redraw();
}

static void set_volume(int pct) {
  pct = std::max(0, std::min(150, pct));
#ifdef FLD_HAVE_AUDIO
  if (g_audio) {
    if (const Audio::Device *d = g_audio->defaultDevice(Audio::Sink)) g_audio->setDeviceLevel(d->id, pct / 100.0f);
    P.volume_pct = pct;
    update_volume_widget();
    return;
  }
#endif
  char v[16];
  std::string out;
  if (have_wpctl()) {
    snprintf(v, sizeof v, "%d%%", pct);
    fld::run_capture({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", v}, out);
  } else if (have_pactl()) {
    snprintf(v, sizeof v, "%d%%", pct);
    fld::run_capture({"pactl", "set-sink-volume", "@DEFAULT_SINK@", v}, out);
  }
  P.volume_pct = pct;
  update_volume_widget();
}

static void toggle_mute() {
#ifdef FLD_HAVE_AUDIO
  if (g_audio) {
    g_audio->toggleDefaultMute(Audio::Sink);
    return;
  }
#endif
  std::string out;
  if (have_wpctl()) fld::run_capture({"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"}, out);
  else if (have_pactl()) fld::run_capture({"pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle"}, out);
  P.muted = !P.muted;
  update_volume_widget();
}

class VolumeButton : public fld::FlatButton {
public:
  using fld::FlatButton::FlatButton;
  int handle(int e) override {
    if (e == FL_MOUSEWHEEL) {
      set_volume(P.volume_pct - Fl::event_dy() * 5);
      return 1;
    }
    if (e == FL_PUSH && Fl::event_button() == FL_MIDDLE_MOUSE) {
      toggle_mute();
      return 1;
    }
    return fld::FlatButton::handle(e);
  }
};

static void volume_menu(fld::Menu &m) {
  m.toggle("Mute", P.muted, [] { toggle_mute(); }).divider();
  m.item("Sound Settings…", [] { fld::spawn({"fl-settings", "sound"}); });
  m.item("Volume Mixer…", [] { fld::spawn({"fl-mixer"}); });
}

static void volume_cb(Fl_Widget *w, void *) {
  PanelPopup pop(260, 96);
  Fl_Slider *s = new Fl_Slider(12, 14, 180, 22);
  s->type(FL_HOR_NICE_SLIDER);
  s->bounds(0, 100);
  s->step(1);
  s->value(std::min(100, P.volume_pct));
  s->callback([](Fl_Widget *sl, void *) { set_volume((int)((Fl_Slider *)sl)->value()); });
  s->when(FL_WHEN_CHANGED);
  Fl_Check_Button *m = new Fl_Check_Button(200, 12, 56, 26, "Mute");
  m->value(P.muted);
  m->callback([](Fl_Widget *, void *) { toggle_mute(); });
  Fl_Box *more = new Fl_Box(12, 40, 236, 20);
  more->labelsize(11);
  more->labelcolor(fld::palette().fg_dim);
  more->label("Scroll on the icon to adjust, middle-click to mute");
  Fl_Button *settings = new Fl_Button(12, 62, 116, 26, "Sound Settings");
  settings->labelsize(12);
  settings->callback([](Fl_Widget *b, void *) {
    fld::spawn({"fl-settings", "sound"});
    b->window()->hide();
  });
  Fl_Button *mixer = new Fl_Button(132, 62, 116, 26, "Volume Mixer");
  mixer->labelsize(12);
  mixer->callback([](Fl_Widget *b, void *) {
    fld::spawn({"fl-mixer"});
    b->window()->hide();
  });
  pop.end();
  pop.popup_at(w, P.top);
}

static void clock_cb(Fl_Widget *w, void *) {
  PanelPopup pop(260, 220);
  new Calendar(1, 1, 258, 218);
  pop.end();
  pop.popup_at(w, P.top);
}

static void launcher_cb(Fl_Widget *, void *) { fld::spawn({"fl-launcher"}); }

// QuickLaunch in panel.ini; the panel restarts when it changes
static std::vector<std::string> quick_ids() {
  fld::Ini c;
  c.load(fld::fld_config_dir() + "/panel.ini");
  std::vector<std::string> r;
  for (auto &x : fld::split(c.get("Panel", "QuickLaunch", "fl-files;fl-term;fl-edit"), ';')) {
    std::string t = fld::trim(x);
    if (fld::ends_with(t, ".desktop")) t.resize(t.size() - 8);
    if (!t.empty()) r.push_back(t);
  }
  return r;
}

static void set_quick_ids(const std::vector<std::string> &ids) {
  fld::Ini c;
  std::string path = fld::fld_config_dir() + "/panel.ini";
  c.load(path);
  c.set("Panel", "QuickLaunch", fld::join(ids, ";"));
  c.save(path);
}

static void quick_menu(fld::DesktopEntry *e, fld::Menu &m) {
  std::string id = e->id;
  if (fld::ends_with(id, ".desktop")) id.resize(id.size() - 8);
  auto ids = quick_ids();
  int i = (int)(std::find(ids.begin(), ids.end(), id) - ids.begin());
  int n = (int)ids.size();
  m.item("Open", [e] { e->launch(); });
  for (auto &a : e->actions) {
    std::string aid = a.id;
    m.item(fld::Menu::text(a.name), [e, aid] { e->launch_action(aid); });
  }
  m.divider();
  m.item("Move Left", [i] {
    auto l = quick_ids();
    if (i > 0 && i < (int)l.size()) std::swap(l[(size_t)i], l[(size_t)i - 1]);
    set_quick_ids(l);
  }, i > 0 && i < n ? 0 : FL_MENU_INACTIVE);
  m.item("Move Right", [i] {
    auto l = quick_ids();
    if (i + 1 < (int)l.size()) std::swap(l[(size_t)i], l[(size_t)i + 1]);
    set_quick_ids(l);
  }, i + 1 < n ? 0 : FL_MENU_INACTIVE);
  m.item("Remove from Quick Launch", [id] {
    auto l = quick_ids();
    l.erase(std::remove(l.begin(), l.end(), id), l.end());
    set_quick_ids(l);
  });
  m.divider();
  panel_menu_items(m);
}

static void clock_menu(fld::Menu &m) {
  auto copy = [](const char *fmt) {
    char buf[128];
    time_t t = time(nullptr);
    strftime(buf, sizeof buf, fmt, localtime(&t));
    Fl::copy(buf, (int)strlen(buf), 1);
  };
  m.item("Copy Date", [copy] { copy("%Y-%m-%d"); });
  m.item("Copy Time", [copy] { copy("%H:%M:%S"); });
  m.item("Copy Date and Time", [copy] { copy("%c"); }).divider();
  m.item("Date && Time Settings…", [] { fld::spawn({"fl-settings", "datetime"}); });
  m.item("Clock Format…", [] { fld::spawn({"fl-settings", "panel"}); });
}

static void volume_menu(fld::Menu &m);

static void quick_cb(Fl_Widget *w, void *) {
  fld::DesktopEntry *e = (fld::DesktopEntry *)w->user_data();
  e->launch();
}

static void power_cb(Fl_Widget *, void *) {
  using fld::PowerAction;
  fld::Menu m;
  for (PowerAction a : {PowerAction::Lock, PowerAction::LogOut, PowerAction::Suspend, PowerAction::Reboot,
                        PowerAction::PowerOff}) {
    int flags = a == PowerAction::Lock && !fld::can_lock() ? FL_MENU_INACTIVE : 0;
    m.item(fld::power_label(a), [a] { fld::confirm_power(a); }, flags);
    if (a == PowerAction::LogOut) m.divider();
  }
  m.popup();
}

// True if a real keyboard is connected (a key device with auto repeat and the
// kbd handler in /proc/bus/input/devices); virtual keyboards are not counted.
static bool have_physical_keyboard() {
  std::string data;
  if (!fld::read_file("/proc/bus/input/devices", data)) return true;
  bool kbd = false, virt = false;
  unsigned long ev = 0;
  for (auto &line : fld::split(data + "\n\n", '\n', true)) {
    if (line.empty()) {
      if (kbd && (ev & 0x100000) && !virt) return true;
      kbd = virt = false;
      ev = 0;
    } else if (fld::starts_with(line, "H: Handlers=")) {
      kbd = (" " + line.substr(12) + " ").find(" kbd ") != std::string::npos;
    } else if (fld::starts_with(line, "B: EV=")) {
      ev = strtoul(line.c_str() + 6, nullptr, 16);
    } else if (fld::starts_with(line, "S: Sysfs=")) {
      virt = line.find("/virtual/") != std::string::npos;
    }
  }
  return false;
}

#ifdef FLD_HAVE_NM
#include "network_ui.h"
#include "nmclient.h"

static std::string g_net_tip;

static void network_changed() {
  const nm::State &st = nm::Client::get().state();
  if (!st.running) {
    if (P.network->visible()) { P.network->hide(); relayout(); }
    return;
  }
  const char *icon;
  if (st.primary_type == "802-11-wireless") icon = nm::signal_icon(st.primary_strength);
  else if (!st.primary_type.empty()) icon = "network-wired";
  else if (!st.networking) icon = "network-offline";
  else icon = "network-wired-disconnected";
  bool connecting = false;
  for (auto &d : st.devices) if (d.connecting) connecting = true;
  if (connecting && st.primary_type.empty()) icon = "network-wired-acquiring";
  P.network->icon_name(icon, 18);
  if (!fld::icon(icon, 18)) P.network->icon_name("fl-network", 18);
  g_net_tip = st.primary_id.empty() ? (st.networking ? "Not connected" : "Networking is off")
                                    : "Connected to " + st.primary_id;
  for (auto &d : st.devices)
    if (d.connected && !d.ip4.empty()) { g_net_tip += "\n" + d.iface + ": " + d.ip4[0]; break; }
  for (auto &c : st.connections)
    if (c.active && (c.type == "vpn" || c.type == "wireguard")) g_net_tip += "\nVPN: " + c.id;
  P.network->tooltip(g_net_tip.c_str());
  if (!P.network->visible()) { P.network->show(); relayout(); }
  P.network->redraw();
}

#endif

void relayout() {
  if (!P.win) return;
  int W = P.win->w(), H = P.win->h();
  int x = 4;
  P.menu_btn->resize(x, 2, P.menu_btn->w(), H - 4);
  x += P.menu_btn->w() + 4;
  P.quick->resize(x, 2, P.quick->children() * 36, H - 4);
  int qx = x;
  for (int i = 0; i < P.quick->children(); i++) P.quick->child(i)->resize(qx + i * 36, 2, 34, H - 4);
  x += P.quick->w() + 8;
  if (P.pager && P.pager->visible()) {
    P.pager->resize(x, 2, P.pager->w(), H - 4);
    x += P.pager->w() + 8;
  }
  // right side, right to left
  int r = W - 4;
  P.power->resize(r - 34, 2, 34, H - 4);
  r -= 36;
  if (P.bell && P.bell->visible()) {
    P.bell->resize(r - 34, 2, 34, H - 4);
    r -= 36;
  }
  r -= 2;
  fl_font(FL_HELVETICA, 14);
  int cw = (int)fl_width(P.clock->label() ? P.clock->label() : "00:00") + 24;
  P.clock->resize(r - cw, 2, cw, H - 4);
  r -= cw + 2;
  if (P.battery->visible()) {
    int bw = (int)fl_width(P.battery->label() ? P.battery->label() : "100%") + 40;
    P.battery->resize(r - bw, 2, bw, H - 4);
    r -= bw + 2;
  }
  for (fld::FlatButton *b : {P.clipboard, P.printer, P.bluetooth}) {
    if (b && b->visible()) {
      b->resize(r - 34, 2, 34, H - 4);
      r -= 36;
    }
  }
  if (P.network && P.network->visible()) {
    P.network->resize(r - 34, 2, 34, H - 4);
    r -= 36;
  }
  if (P.keyboard && P.keyboard->visible()) {
    P.keyboard->resize(r - 34, 2, 34, H - 4);
    r -= 36;
  }
  if (P.volume->visible()) {
    P.volume->resize(r - 34, 2, 34, H - 4);
    r -= 36;
  }
  P.tasks->resize(x, 2, std::max(10, r - x - 8), H - 4);
  relayout_tasks();
  P.win->redraw();
}

// ---- right-click menus ----------------------------------------------------------------------

static std::map<Fl_Widget *, std::function<void(fld::Menu &)>> &context_menus() {
  static std::map<Fl_Widget *, std::function<void(fld::Menu &)>> m;
  return m;
}

void panel_context_menu(Fl_Widget *w, std::function<void(fld::Menu &)> fill) { context_menus()[w] = std::move(fill); }

void panel_menu_items(fld::Menu &m) {
  m.item("Panel Settings…", [] { fld::spawn({"fl-settings", "panel"}); });
}

// the panel's own menu (a right click on an empty part)
static void panel_menu() {
  fld::Menu m;
  m.item("Task Manager…", [] { fld::spawn({"fl-taskmgr"}); });
  m.item("Applications…", [] { fld::spawn({"fl-launcher"}); }).divider();
  m.item("Lock Screen", [] { fld::lock_screen(); }, fld::can_lock() ? 0 : FL_MENU_INACTIVE).divider();
  panel_menu_items(m);
  m.item("Settings…", [] { fld::spawn({"fl-settings"}); });
  m.popup();
}

// the innermost visible widget under x, y
static Fl_Widget *panel_widget_at(Fl_Group *g, int x, int y) {
  for (int i = g->children() - 1; i >= 0; i--) {
    Fl_Widget *c = g->child(i);
    if (!c->visible() || x < c->x() || y < c->y() || x >= c->x() + c->w() || y >= c->y() + c->h()) continue;
    if (Fl_Group *cg = c->as_group()) {
      Fl_Widget *in = panel_widget_at(cg, x, y);
      return in ? in : c;
    }
    return c;
  }
  return nullptr;
}

class PanelWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  int handle(int e) override {
    if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      Fl_Widget *w = panel_widget_at(this, Fl::event_x(), Fl::event_y());
      auto it = w ? context_menus().find(w) : context_menus().end();
      if (it != context_menus().end()) {
        fld::Menu m;
        it->second(m);
        m.popup();
        return 1;
      }
      // task buttons have their own menu
      if (!w || w == P.tasks || w == P.quick) {
        panel_menu();
        return 1;
      }
    }
    return Fl_Double_Window::handle(e);
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Double_Window::resize(X, Y, W, H);
    relayout();
  }
  void draw() override {
    Fl_Double_Window::draw();
    // thin separator line towards the screen
    fl_color(fl_color_average(panel_bg(), FL_WHITE, 0.8f));
    if (P.top) fl_xyline(0, h() - 1, w());
    else fl_xyline(0, 0, w());
  }
};

static int volume_ticks = 0;

static void tick(void *) {
  update_clock();
  if (++volume_ticks % 3 == 0) {
    query_volume();
    update_volume_widget();
  }
#ifdef FLD_HAVE_POWER
  if (volume_ticks % 30 == 1 && !power_indicator_active()) update_battery();
#else
  if (volume_ticks % 30 == 1) update_battery();
#endif
  Fl::repeat_timeout(1.0, tick);
}

fld::FlatButton *make_btn(int w, const char *icon, const char *tip, Fl_Callback *cb) {
  auto *b = new fld::FlatButton(0, 0, w, PANEL_H - 4);
  b->color(panel_bg());
  b->labelcolor(panel_fg());
  b->labelsize(14);
  b->hover_color(panel_hover());
  if (icon) b->icon_name(icon, 20);
  if (tip) b->copy_tooltip(tip);
  b->callback(cb);
  return b;
}

// Everything that the panel reads at start; when it changes the panel restarts.
static std::string settings_signature() {
  fld::Ini p, d, w;
  p.load(fld::fld_config_dir() + "/panel.ini");
  d.load(fld::desktop_config_path());
  w.load(fld::fld_config_dir() + "/wm.ini");
  std::string sig = p.to_string() + "|" + w.get("behaviour", "panel_position", "bottom");
  for (const char *k : {"Theme", "Accent", "Font", "FontSize", "Scheme"}) sig += "|" + d.get("Appearance", k);
  return sig + "|" + d.get("General", "IconTheme");
}

int main(int argc, char **argv) {
  fld::init_app("fldesktop.panel", argc, argv);
  // The shell window is borderless and fl-wm knows it by its title; any other
  // window (dialogs, fl_choice) gets an ordinary app id, or fl-wm would give
  // it the shell role too.
  Fl_Window::default_xclass("fl-panel");
  static int s_argc = argc;
  static char **s_argv = argv;
  static std::string s_sig = settings_signature();
  auto check_restart = [] {
    if (settings_signature() != s_sig) fld::restart_self(s_argc, s_argv);
  };
  static fld::ConfigWatcher w1(fld::fld_config_dir() + "/panel.ini", check_restart);
  static fld::ConfigWatcher w2(fld::desktop_config_path(), check_restart);
  static fld::ConfigWatcher w3(fld::fld_config_dir() + "/wm.ini", check_restart);
  fld::Ini cfg;
  cfg.load(fld::fld_config_dir() + "/panel.ini");
  std::string wm_pos;
  {
    fld::Ini wm;
    wm.load(fld::fld_config_dir() + "/wm.ini");
    wm_pos = fld::lower(wm.get("behaviour", "panel_position", "bottom"));
  }
  P.top = wm_pos == "top";
  P.clock_fmt = cfg.get("Clock", "Format", P.clock_fmt);
  std::string quick = cfg.get("Panel", "QuickLaunch", "fl-files;fl-term;fl-edit");
  for (auto &d : fld::list_dir("/sys/class/power_supply")) {
    std::string type;
    fld::read_file("/sys/class/power_supply/" + d + "/type", type);
    if (fld::trim(type) == "Battery") { P.bat_path = "/sys/class/power_supply/" + d; break; }
  }

  P.win = new PanelWindow(1024, PANEL_H, "fldesktop.panel");
  P.win->color(panel_bg());
  P.win->box(FL_FLAT_BOX);
  P.win->border(0);

  P.menu_btn = make_btn(96, "start-here", "Applications (Super+Space)", launcher_cb);
  P.menu_btn->copy_label("Apps");
  if (!fld::icon("start-here", 20)) P.menu_btn->icon_name("view-app-grid-symbolic", 20);
  if (!fld::icon("start-here", 20) && !fld::icon("view-app-grid-symbolic", 20))
    P.menu_btn->icon_name("applications-other", 20);

  panel_context_menu(P.menu_btn, [](fld::Menu &m) {
    m.item("Open Applications", [] { fld::spawn({"fl-launcher"}); }).divider();
    m.item("Task Manager…", [] { fld::spawn({"fl-taskmgr"}); });
    m.item("Settings…", [] { fld::spawn({"fl-settings"}); }).divider();
    panel_menu_items(m);
  });

  P.quick = new Fl_Group(0, 0, 10, PANEL_H);
  static std::vector<std::unique_ptr<fld::DesktopEntry>> quick_entries;
  for (auto &id : fld::split(quick, ';')) {
    auto e = std::make_unique<fld::DesktopEntry>();
    if (!fld::find_application(fld::trim(id), *e)) continue;
    auto *b = make_btn(34, nullptr, e->name.c_str(), quick_cb);
    b->icon_name(e->icon.c_str(), 22);
    b->user_data(e.get());
    fld::DesktopEntry *ep = e.get();
    panel_context_menu(b, [ep](fld::Menu &m) { quick_menu(ep, m); });
    quick_entries.push_back(std::move(e));
  }
  P.quick->end();
#ifdef FLD_HAVE_PAGER
  // the workspace pager: Pager=auto (when fl-wm has several workspaces) | never
  if (fld::lower(cfg.get("Panel", "Pager", "auto")) != "never") P.pager = pager_create();
#endif

  P.tasks = new Fl_Group(0, 0, 10, PANEL_H);
  P.tasks->end();
  P.tasks->box(FL_NO_BOX);

  P.volume = new VolumeButton(0, 0, 34, PANEL_H - 4);
  P.volume->color(panel_bg());
  P.volume->hover_color(panel_hover());
  P.volume->callback(volume_cb);
  panel_context_menu(P.volume, volume_menu);
  P.volume->hide();
  // on-screen keyboard button: KeyboardButton=auto (no physical keyboard) | always | never
  std::string kb_mode = fld::lower(cfg.get("Panel", "KeyboardButton", "auto"));
  if (kb_mode == "always" || (kb_mode == "auto" && !have_physical_keyboard())) {
    P.keyboard = make_btn(34, "input-keyboard", "On-screen keyboard",
                          [](Fl_Widget *, void *) { fld::spawn({"fl-osk"}); });
    if (!fld::icon("input-keyboard", 20)) P.keyboard->copy_label("⌨");
    panel_context_menu(P.keyboard, [](fld::Menu &m) {
      m.item("Show On-screen Keyboard", [] { fld::spawn({"fl-osk"}); });
      m.item("Keyboard Settings…", [] { fld::spawn({"fl-settings", "keyboard"}); }).divider();
      panel_menu_items(m);
    });
  }
#ifdef FLD_HAVE_NM
  P.network = make_btn(34, "network-wired", "Network", [](Fl_Widget *w, void *) { network_popup(w); });
  panel_context_menu(P.network, network_context_menu);
  P.network->hide();
#endif
#ifdef FLD_HAVE_BLUETOOTH
  Fl::lock();
  P.bluetooth = bluetooth_indicator_create();
#endif
#ifdef FLD_HAVE_PRINTERS
  P.printer = printer_indicator_create();
#endif
  P.clipboard = clipboard_indicator_create();
  P.battery = make_btn(70, "battery-good", "Battery", nullptr);
  P.battery->hide();
#ifdef FLD_HAVE_POWER
  Fl::lock();
  power_indicator_start(P.battery);
#endif
#ifdef FLD_HAVE_UDISKS
  Fl::lock();
  drives_start();
#endif
  P.clock = make_btn(140, nullptr, nullptr, clock_cb);
  panel_context_menu(P.clock, clock_menu);
#ifdef FLD_HAVE_NOTIFY
  Fl::lock();
  P.bell = notify_indicator_create();
#endif
  P.power = make_btn(34, "system-shutdown", "Power", power_cb);
  if (Fl_Image *pi = fld::symbolic_icon("system-shutdown", 18, panel_fg())) P.power->icon_image(pi);
  panel_context_menu(P.power, [](fld::Menu &m) {
    using fld::PowerAction;
    for (PowerAction a : {PowerAction::Lock, PowerAction::LogOut, PowerAction::Suspend, PowerAction::Reboot,
                          PowerAction::PowerOff}) {
      m.item(fld::power_label(a), [a] { fld::confirm_power(a); },
             a == PowerAction::Lock && !fld::can_lock() ? FL_MENU_INACTIVE : 0);
      if (a == PowerAction::LogOut) m.divider();
    }
    m.divider();
    m.item("Power Settings…", [] { fld::spawn({"fl-settings", "power"}); });
  });
  P.win->end();
  P.win->resizable(P.tasks);
  P.win->size_range(200, PANEL_H, 0, PANEL_H);

  update_clock();
#ifdef FLD_HAVE_AUDIO
  Fl::lock();
  g_audio = AudioBackend::create();
  if (g_audio) {
    g_audio->on_changed = audio_changed;
    g_audio->on_connection = [](bool) { audio_changed(); };
  }
#endif
#ifdef FLD_HAVE_NM
  Fl::lock();
  nm::Client::get().on_changed = network_changed;
  // the panel also answers NetworkManager's requests for Wi-Fi passwords
  nm::Client::get().start_secret_agent([](const std::string &ssid, std::function<void(const std::string &)> reply) {
    reply(nm::ask_wifi_password(ssid, true));
  });
#endif
  query_volume();
  update_volume_widget();
  update_battery();
  relayout();

  // fl-wm also asks for the full width, but FLTK ignores configure sizes for
  // borderless windows, so follow the screen size ourselves.
  P.win->size(Fl::w(), PANEL_H);
  Fl::add_handler([](int e) -> int {
    if (e == FL_SCREEN_CONFIGURATION_CHANGED) P.win->size(Fl::w(), PANEL_H);
    return 0;
  });
  P.win->show(); // not show(argc, argv): it resets the theme colors
  wl_display *dpy = fl_wl_display();
  if (dpy) {
    wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &kRegListener, nullptr);
    wl_display_roundtrip(dpy);
    if (!g_ftl_mgr) fprintf(stderr, "fl-panel: compositor lacks wlr-foreign-toplevel-management\n");
  } else {
    fprintf(stderr, "fl-panel: needs a Wayland session\n");
  }
  Fl::add_timeout(1.0, tick);
  return Fl::run();
}
