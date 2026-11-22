#include "fld/tab_bar.h"
#include "fld/drag.h"
#include "fld/tab_move.h"
#include "fld/theme.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Tooltip.H>
#include <FL/Fl_Window.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdlib>
#include <unistd.h>

namespace fld {

static std::vector<TabBar *> &bars() {
  static std::vector<TabBar *> v;
  return v;
}
static bool alive(TabBar *b) { return std::find(bars().begin(), bars().end(), b) != bars().end(); }

// the tab being dragged to another window, from this process
static struct Outgoing {
  TabBar *bar = nullptr;
  int index = -1;
  bool local = false; // dropped back on its own strip and moved there
} g_out;

static const int PLUS_W = 32, ARROW_W = 22, MIN_W = 100, MAX_W = 240, DRAG_START = 6;

int moved_index(int cur, int from, int to) {
  if (cur == from) return to;
  if (from < cur && to >= cur) return cur - 1;
  if (from > cur && to <= cur) return cur + 1;
  return cur;
}

int TabBar::drop_index(int from, int drag_x, int tw, int n) {
  (void)from;
  if (n <= 0 || tw <= 0) return 0;
  int c = drag_x + tw / 2;
  int i = c < 0 ? 0 : c / tw;
  return std::max(0, std::min(n - 1, i));
}

TabBar::TabBar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
  box(FL_FLAT_BOX);
  clear_visible_focus();
  bars().push_back(this);
  drag_setup_later(); // a tab drag needs the press that starts it
}

TabBar::~TabBar() {
  bars().erase(std::remove(bars().begin(), bars().end(), this), bars().end());
  if (g_out.bar == this) g_out = Outgoing();
}

TabBar *TabBar::in_window(Fl_Window *w, const std::string &app) {
  if (!w) return nullptr;
  w = w->top_window();
  for (auto *b : bars())
    if (b->window() && b->window()->top_window() == w && (app.empty() || b->app_id == app)) return b;
  return nullptr;
}

int TabBar::insert_index(int ex) const {
  int n = (int)tabs_.size();
  int tw = tab_width();
  if (n == 0 || tw <= 0) return 0;
  int c = ex - first_x() + tw / 2;
  return std::max(0, std::min(n, c / tw));
}

bool TabBar::take_drop(const std::string &file, int ex, int ey) {
  if (g_out.bar == this) {
    // our own tab: onto the strip moves it there, elsewhere it tears off
    if (!over(ey)) return false;
    discard_tab_file(file);
    g_out.local = true;
    int n = (int)tabs_.size(), from = g_out.index, at = insert_index(ex);
    int to = std::max(0, std::min(n - 1, at > from ? at - 1 : at));
    if (to != from && from >= 0 && from < n && on_move) on_move(from, to);
    return true;
  }
  if (!on_import) return false;
  std::string state;
  int pid = 0;
  if (!claim_tab_file(file, app_id, state, &pid)) return false;
  int at = over(ey) ? insert_index(ex) : (int)tabs_.size();
  if (!on_import(state, at, pid == (int)getpid())) {
    restore_tab_file(file, app_id, state, pid);
    return false;
  }
  return true;
}

bool TabBar::start_tab_drag(int i) {
  if (!on_export || app_id.empty() || i < 0 || i >= (int)tabs_.size() || !window()) return false;
  std::string state = on_export(i);
  if (state.empty()) return false;
  std::string label = tabs_[(size_t)i].label.empty() ? empty_label : tabs_[(size_t)i].label;
  std::string file = write_tab_file(app_id, state, label);
  if (file.empty()) return false;
  DragImage img;
  img.label = label;
  TabBar *self = this;
  int n_before = (int)tabs_.size();
  bool ok = start_drag(window(), kTabPayloadPrefix + file, false, img, [self, i, file, n_before](DragEnd) {
    bool local = g_out.local && g_out.bar == self;
    if (g_out.bar == self) g_out = Outgoing();
    if (!alive(self)) { discard_tab_file(file); return; }
    self->out_ = -1;
    self->redraw();
    if (local) return;
    if (i >= self->count()) { discard_tab_file(file); return; } // the tabs changed meanwhile
    if (!file_exists(file)) { // another window took it
      if (self->on_moved_away) self->on_moved_away(i);
      return;
    }
    if (n_before < 2) { discard_tab_file(file); return; } // the only tab stays
    if (self->on_tear_off) self->on_tear_off(i, file);
    else {
      discard_tab_file(file);
      if (self->on_detach) self->on_detach(i);
    }
  });
  if (!ok) {
    discard_tab_file(file);
    return false;
  }
  g_out.bar = this;
  g_out.index = i;
  g_out.local = false;
  out_ = i;
  press_ = -1;
  dragging_ = outside_ = false;
  redraw();
  return true;
}

// ---- the drop hook: a dropped tab payload goes to the window's strip, not
// to the widget under the pointer (FLTK pastes a drop into Fl::belowmouse())

namespace {

class Catcher : public Fl_Widget {
public:
  Fl_Widget *real = nullptr;
  Fl_Window *win = nullptr;
  Catcher() : Fl_Widget(0, 0, 0, 0) {}
  void draw() override {}
  int handle(int e) override {
    Fl_Widget *r = real;
    Fl_Window *w = win;
    real = nullptr;
    win = nullptr;
    if (Fl::belowmouse_ == this) Fl::belowmouse_ = r;
    if (e == FL_PASTE) {
      std::string file;
      if (parse_tab_payload(std::string(Fl::event_text() ? Fl::event_text() : "", Fl::event_length()), file)) {
        if (w)
          for (auto *b : std::vector<TabBar *>(bars()))
            if (alive(b) && b->window() && b->window()->top_window() == w &&
                b->take_drop(file, Fl::event_x(), Fl::event_y()))
              break;
        return 1;
      }
    }
    return r ? r->handle(e) : 0;
  }
};

Catcher *catcher() {
  static Catcher *c = nullptr;
  if (!c) {
    Fl_Group *cur = Fl_Group::current();
    Fl_Group::current(nullptr);
    c = new Catcher;
    Fl_Group::current(cur);
  }
  return c;
}

Fl_Event_Dispatch g_prev_dispatch = nullptr;

bool takes_tabs(Fl_Window *top) {
  for (auto *b : bars())
    if (b->window() && b->window()->top_window() == top && (b->on_import || g_out.bar == b)) return true;
  return false;
}

int tab_dispatch(int e, Fl_Window *w) {
  int r = g_prev_dispatch ? g_prev_dispatch(e, w) : Fl::handle_(e, w);
  if (w && (e == FL_DND_ENTER || e == FL_DND_DRAG || e == FL_DND_RELEASE)) {
    Fl_Window *top = w->top_window();
    if (takes_tabs(top)) {
      r = 1; // anywhere in a window with tabs: a tab may be dropped
      if (e == FL_DND_RELEASE) {
        Catcher *c = catcher();
        c->real = Fl::belowmouse() == c ? nullptr : Fl::belowmouse();
        c->win = top;
        Fl::belowmouse_ = c;
      }
    }
  }
  return r;
}

} // namespace

void install_tab_drop_hook() {
  static bool done = false;
  if (done) return;
  done = true;
  g_prev_dispatch = Fl::event_dispatch();
  Fl::event_dispatch(tab_dispatch);
}

int TabBar::preferred_height() { return std::max(28, ui_font_size() * 2 + 4); }

void TabBar::set_tabs(const std::vector<TabInfo> &tabs, int active) {
  tabs_ = tabs;
  active_ = std::max(0, std::min(active, (int)tabs_.size() - 1));
  if (hover_ >= (int)tabs_.size()) hover_ = -1;
  // (a press stays: selecting the pressed tab calls set_tabs before a drag)
  if (press_ >= (int)tabs_.size()) press_ = -1, dragging_ = false;
  keep_visible(active_);
  redraw();
}

void TabBar::set_tabs(const std::vector<std::string> &labels, int active) {
  std::vector<TabInfo> t;
  for (auto &l : labels) t.push_back({l, "", false, false});
  set_tabs(t, active);
}

void TabBar::resize(int X, int Y, int W, int H) {
  Fl_Widget::resize(X, Y, W, H);
  keep_visible(active_);
}

bool TabBar::overflow() const { return (int)tabs_.size() * MIN_W > w() - PLUS_W; }
int TabBar::arrows_w() const { return overflow() ? 2 * ARROW_W : 0; }

int TabBar::tab_width() const {
  int n = std::max(1, (int)tabs_.size());
  int avail = w() - PLUS_W - arrows_w();
  return std::max(MIN_W, std::min(MAX_W, avail / n));
}

int TabBar::visible_count() const {
  int avail = w() - PLUS_W - arrows_w();
  return std::max(1, avail / tab_width());
}

int TabBar::first_x() const { return x() - scroll_ * tab_width(); }

void TabBar::keep_visible(int i) {
  int n = (int)tabs_.size();
  if (!overflow()) {
    scroll_ = 0;
    return;
  }
  int vis = visible_count();
  if (i < scroll_) scroll_ = i;
  if (i >= scroll_ + vis) scroll_ = i - vis + 1;
  scroll_ = std::max(0, std::min(scroll_, n - vis));
}

void TabBar::tab_rect(int i, int &tx, int &tw) const {
  tw = tab_width();
  tx = first_x() + i * tw;
}

void TabBar::close_rect(int i, int &cx, int &cy, int &cs) const {
  int tx, tw;
  tab_rect(i, tx, tw);
  cs = std::min(18, h() - 8);
  cx = tx + tw - cs - 6;
  cy = y() + (h() - cs) / 2;
}

void TabBar::plus_rect(int &px, int &pw) const {
  pw = PLUS_W;
  if (overflow()) px = x() + w() - PLUS_W;
  else px = x() + (int)tabs_.size() * tab_width();
}

int TabBar::tab_at(int ex, int ey, bool &on_close) const {
  on_close = false;
  if (ey < y() || ey >= y() + h()) return -1;
  if (ex >= x() + w() - PLUS_W - arrows_w()) return -1;
  for (int i = 0; i < (int)tabs_.size(); i++) {
    int tx, tw;
    tab_rect(i, tx, tw);
    if (ex >= tx && ex < tx + tw) {
      int cx, cy, cs;
      close_rect(i, cx, cy, cs);
      on_close = ex >= cx - 2 && ex < cx + cs + 2 && ey >= cy - 2 && ey < cy + cs + 2;
      return i;
    }
  }
  return -1;
}

static std::string fit(std::string s, int maxw) {
  if (maxw <= 0) return "";
  if ((int)fl_width(s.c_str()) <= maxw) return s;
  while (!s.empty() && (int)fl_width((s + "…").c_str()) > maxw) {
    size_t k = s.size() - 1;
    while (k > 0 && ((unsigned char)s[k] & 0xc0) == 0x80) k--;
    s.erase(k);
  }
  return s + "…";
}

void TabBar::draw_tab(int i, int tx, int tw, Fl_Color bar, bool lifted) {
  const Palette &p = palette();
  const TabInfo &t = tabs_[(size_t)i];
  bool ghost = i == out_; // being dragged to another window
  bool act = i == active_ && !ghost;
  if (act || i == hover_ || lifted) {
    fl_color(act || lifted ? p.bg : fl_color_average(p.bg, bar, 0.5f));
    fl_rectf(tx, y(), tw, h());
  }
  if (act) {
    fl_color(p.accent);
    fl_rectf(tx, y() + h() - 3, tw, 3);
  }
  if (lifted) {
    fl_color(p.border);
    fl_rect(tx, y(), tw, h());
  } else {
    fl_color(p.border);
    fl_yxline(tx + tw - 1, y() + 6, y() + h() - 6);
  }
  int fs = ui_font_size();
  int cx = tx + tw - std::min(18, h() - 8) - 6, cs = std::min(18, h() - 8), cy = y() + (h() - cs) / 2;
  int lx = tx + 10;
  if (t.busy) {
    fl_color(p.accent);
    fl_line_style(FL_SOLID, 2);
    fl_arc(lx, y() + h() / 2 - 6, 12, 12, 0, 300);
    fl_line_style(0);
    lx += 18;
  }
  if (t.modified) {
    fl_color(act ? p.accent : p.fg_dim);
    fl_pie(lx, y() + h() / 2 - 4, 8, 8, 0, 360);
    lx += 14;
  }
  fl_font(act ? FL_HELVETICA_BOLD : FL_HELVETICA, fs);
  // on a tinted strip the other tabs' labels need a light color
  Fl_Color dim = tint ? fl_color_average(FL_WHITE, tint, 0.75f) : p.fg_dim;
  fl_color(ghost ? p.border : act || lifted ? p.fg : (i == hover_ ? p.fg : dim));
  std::string lab = fit(t.label.empty() ? empty_label : t.label, cx - lx - 4);
  // '@' would start a symbol
  std::string esc;
  for (char c : lab) {
    if (c == '@') esc += '@';
    esc += c;
  }
  fl_draw(esc.c_str(), lx, y(), cx - lx, h(), FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP, nullptr, 0);
  bool hc = i == hover_ && hover_close_ && !dragging_;
  if (hc) {
    fl_color(fl_color_average(p.danger, p.bg, 0.8f));
    fl_rounded_rectf(cx, cy, cs, cs, 4);
  }
  if (act || i == hover_ || lifted) {
    fl_color(hc ? p.danger : p.fg_dim);
    fl_line_style(FL_SOLID, 2);
    int m = cs / 3;
    fl_line(cx + m, cy + m, cx + cs - m, cy + cs - m);
    fl_line(cx + cs - m, cy + m, cx + m, cy + cs - m);
    fl_line_style(0);
  }
}

void TabBar::draw() {
  const Palette &p = palette();
  Fl_Color bar = tint ? tint : fl_color_average(p.bg, FL_BLACK, p.dark ? 0.75f : 0.9f);
  fl_color(bar);
  fl_rectf(x(), y(), w(), h());
  int n = (int)tabs_.size();
  int tw = tab_width();
  int area = w() - PLUS_W - arrows_w();
  fl_push_clip(x(), y(), area, h());
  if (dragging_ && press_ >= 0 && press_ < n && !outside_) {
    // the others make room where the dragged tab would land
    int to = drop_index(press_, drag_x_, tw, n);
    int slot = 0;
    for (int i = 0; i < n; i++) {
      if (i == press_) continue;
      if (slot == to) slot++;
      draw_tab(i, first_x() + slot * tw, tw, bar, false);
      slot++;
    }
    draw_tab(press_, first_x() + drag_x_, tw, bar, true);
  } else {
    for (int i = 0; i < n; i++) {
      int tx, w2;
      tab_rect(i, tx, w2);
      if (tx + w2 <= x() || tx >= x() + area) continue;
      if (dragging_ && i == press_) continue; // dragged out of the strip
      draw_tab(i, tx, w2, bar, false);
    }
  }
  if (dnd_insert_ >= 0) {
    // where a tab dragged here would land
    int cx = first_x() + dnd_insert_ * tw;
    fl_color(p.accent);
    fl_rectf(std::max(x(), cx - 2), y() + 3, 4, h() - 6);
  }
  fl_pop_clip();
  if (overflow()) {
    int ax = x() + area;
    fl_color(scroll_ > 0 ? p.fg : p.border);
    fl_draw("@<", ax, y(), ARROW_W, h(), FL_ALIGN_CENTER);
    fl_color(scroll_ + visible_count() < n ? p.fg : p.border);
    fl_draw("@>", ax + ARROW_W, y(), ARROW_W, h(), FL_ALIGN_CENTER);
  }
  int px, pw;
  plus_rect(px, pw);
  if (hover_plus_) {
    fl_color(fl_color_average(p.bg, bar, 0.5f));
    fl_rounded_rectf(px + 3, y() + 3, pw - 6, h() - 6, 4);
  }
  fl_color(tint ? fl_color_average(FL_WHITE, tint, 0.75f) : p.fg_dim);
  fl_line_style(FL_SOLID, 2);
  int cxp = px + pw / 2, cyp = y() + h() / 2, r = std::min(6, h() / 4);
  fl_line(cxp - r, cyp, cxp + r, cyp);
  fl_line(cxp, cyp - r, cxp, cyp + r);
  fl_line_style(0);
}

int TabBar::handle(int e) {
  int ex = Fl::event_x(), ey = Fl::event_y();
  switch (e) {
    case FL_ENTER:
      return 1;
    case FL_MOVE:
    case FL_LEAVE: {
      bool oc = false;
      int t = e == FL_LEAVE ? -1 : tab_at(ex, ey, oc);
      int px, pw;
      plus_rect(px, pw);
      bool hp = e != FL_LEAVE && ex >= px && ex < px + pw;
      if (t != hover_ || oc != hover_close_ || hp != hover_plus_) {
        if (t != hover_) {
          if (t >= 0 && !tabs_[(size_t)t].tooltip.empty()) {
            int tx, tw;
            tab_rect(t, tx, tw);
            Fl_Tooltip::enter_area(this, tx - x(), 0, tw, h(), tabs_[(size_t)t].tooltip.c_str());
          } else {
            Fl_Tooltip::enter_area(this, 0, 0, 0, 0, nullptr);
          }
        }
        hover_ = t;
        hover_close_ = oc;
        hover_plus_ = hp;
        redraw();
      }
      return 1;
    }
    case FL_PUSH: {
      bool oc = false;
      int t = tab_at(ex, ey, oc);
      int b = Fl::event_button();
      if (b == FL_RIGHT_MOUSE) {
        if (on_menu) on_menu(t);
        return 1;
      }
      int px, pw;
      plus_rect(px, pw);
      if (t < 0 && ex >= px && ex < px + pw) {
        if (on_new) on_new();
        return 1;
      }
      if (t < 0 && overflow() && ex >= x() + w() - PLUS_W - arrows_w()) {
        int n = (int)tabs_.size();
        bool left = ex < x() + w() - PLUS_W - ARROW_W;
        scroll_ = std::max(0, std::min(n - visible_count(), scroll_ + (left ? -1 : 1)));
        redraw();
        return 1;
      }
      if (t < 0) {
        if (Fl::event_clicks() && b == FL_LEFT_MOUSE && on_new) on_new();
        return 1;
      }
      if (b == FL_MIDDLE_MOUSE || (oc && b == FL_LEFT_MOUSE)) {
        if (on_close) on_close(t);
        return 1;
      }
      if (b == FL_LEFT_MOUSE) {
        int tx, tw;
        tab_rect(t, tx, tw);
        press_ = t;
        press_x_ = ex;
        grab_dx_ = ex - tx;
        dragging_ = outside_ = false;
        if (t != active_ && on_select) on_select(t);
      }
      return 1;
    }
    case FL_DRAG: {
      if (press_ < 0 || press_ >= (int)tabs_.size()) return 1;
      if (!dragging_ && std::abs(ex - press_x_) < DRAG_START && ey >= y() && ey < y() + h()) return 1;
      if (!dragging_ && (on_move || on_detach)) dragging_ = true;
      if (!dragging_) return 1;
      drag_x_ = ex - grab_dx_ - first_x();
      bool far = ey < y() - 40 || ey > y() + h() + 40;
      // out of the strip: a real drag, which other windows can take
      if (far && on_export && start_tab_drag(press_)) return 1;
      outside_ = on_detach && (int)tabs_.size() > 1 && far;
      redraw();
      return 1;
    }
    case FL_RELEASE: {
      int from = press_;
      bool was = dragging_, out = outside_;
      press_ = -1;
      dragging_ = outside_ = false;
      if (was && from >= 0 && from < (int)tabs_.size()) {
        redraw();
        if (out) {
          if (on_detach) on_detach(from);
        } else {
          int to = drop_index(from, drag_x_, tab_width(), (int)tabs_.size());
          if (to != from && on_move) on_move(from, to);
        }
      }
      return 1;
    }
    case FL_DND_ENTER:
    case FL_DND_DRAG: {
      if (!on_import && g_out.bar != this) return 0;
      int at = insert_index(ex);
      if (at != dnd_insert_) { dnd_insert_ = at; redraw(); }
      return 1;
    }
    case FL_DND_LEAVE:
    case FL_DND_RELEASE:
      if (dnd_insert_ != -1) { dnd_insert_ = -1; redraw(); }
      return 1;
    case FL_PASTE:
      return 1; // a tab payload went to the drop hook; text is not for the strip
    case FL_MOUSEWHEEL: {
      int n = (int)tabs_.size();
      if (n < 2) return 1;
      int d = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
      int t = (active_ + (d > 0 ? 1 : -1) + n) % n;
      if (on_select) on_select(t);
      return 1;
    }
  }
  return Fl_Widget::handle(e);
}

} // namespace fld
