#include "tab_bar.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <algorithm>

#include "fld/theme.h"
#include "fld/widgets.h"

static const int PLUS_W = 32;

TabBar::TabBar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {
  box(FL_FLAT_BOX);
  clear_visible_focus();
}

int TabBar::preferred_height() { return std::max(28, fld::ui_font_size() * 2 + 4); }

void TabBar::set_tabs(const std::vector<std::string> &labels, int active) {
  labels_ = labels;
  active_ = std::max(0, std::min(active, (int)labels_.size() - 1));
  redraw();
}

int TabBar::tab_width() const {
  int n = std::max(1, (int)labels_.size());
  int avail = w() - PLUS_W;
  return std::max(60, std::min(240, avail / n));
}

void TabBar::tab_rect(int i, int &tx, int &tw) const {
  tw = tab_width();
  tx = x() + i * tw;
}

void TabBar::close_rect(int i, int &cx, int &cy, int &cs) const {
  int tx, tw;
  tab_rect(i, tx, tw);
  cs = std::min(18, h() - 8);
  cx = tx + tw - cs - 6;
  cy = y() + (h() - cs) / 2;
}

void TabBar::plus_rect(int &px, int &pw) const {
  int tx, tw;
  tab_rect((int)labels_.size(), tx, tw);
  px = std::min(tx, x() + w() - PLUS_W);
  pw = PLUS_W;
}

int TabBar::tab_at(int ex, int ey, bool &on_close) const {
  on_close = false;
  if (ey < y() || ey >= y() + h()) return -1;
  for (int i = 0; i < (int)labels_.size(); i++) {
    int tx, tw;
    tab_rect(i, tx, tw);
    if (ex >= tx && ex < tx + tw && tx + tw <= x() + w() - PLUS_W + tw) {
      int cx, cy, cs;
      close_rect(i, cx, cy, cs);
      on_close = ex >= cx - 2 && ex < cx + cs + 2 && ey >= cy - 2 && ey < cy + cs + 2;
      return i;
    }
  }
  return -1;
}

void TabBar::draw() {
  const fld::Palette &p = fld::palette();
  Fl_Color bar = fl_color_average(p.bg, FL_BLACK, p.dark ? 0.75f : 0.9f);
  fl_color(bar);
  fl_rectf(x(), y(), w(), h());
  fl_push_clip(x(), y(), w(), h());
  int fs = fld::ui_font_size();
  for (int i = 0; i < (int)labels_.size(); i++) {
    int tx, tw;
    tab_rect(i, tx, tw);
    if (tx >= x() + w() - PLUS_W) break;
    bool act = i == active_;
    if (act || i == hover_) {
      fl_color(act ? p.bg : fl_color_average(p.bg, bar, 0.5f));
      fl_rectf(tx, y(), tw, h());
    }
    if (act) {
      fl_color(p.accent);
      fl_rectf(tx, y() + h() - 3, tw, 3);
    }
    // separator
    fl_color(p.border);
    fl_yxline(tx + tw - 1, y() + 6, y() + h() - 6);
    int cx, cy, cs;
    close_rect(i, cx, cy, cs);
    // the label, cut to fit before the close box
    fl_font(act ? FL_HELVETICA_BOLD : FL_HELVETICA, fs);
    fl_color(act ? p.fg : p.fg_dim);
    std::string lab = labels_[i].empty() ? "Terminal" : labels_[i];
    int maxw = cx - tx - 14;
    if ((int)fl_width(lab.c_str()) > maxw) {
      while (!lab.empty() && (int)fl_width((lab + "…").c_str()) > maxw) {
        // drop a whole UTF-8 character
        size_t k = lab.size() - 1;
        while (k > 0 && ((unsigned char)lab[k] & 0xc0) == 0x80) k--;
        lab.erase(k);
      }
      lab += "…";
    }
    fl_draw(lab.c_str(), tx + 10, y() + (h() + fs) / 2 - 2);
    // close box
    bool hc = i == hover_ && hover_close_;
    if (hc) {
      fl_color(fl_color_average(p.danger, p.bg, 0.8f));
      fl_rounded_rectf(cx, cy, cs, cs, 4);
    }
    if (act || i == hover_) {
      fl_color(hc ? p.danger : p.fg_dim);
      fl_line_style(FL_SOLID, 2);
      int m = cs / 3;
      fl_line(cx + m, cy + m, cx + cs - m, cy + cs - m);
      fl_line(cx + cs - m, cy + m, cx + m, cy + cs - m);
      fl_line_style(0);
    }
  }
  int px, pw;
  plus_rect(px, pw);
  if (hover_plus_) {
    fl_color(fl_color_average(p.bg, bar, 0.5f));
    fl_rounded_rectf(px + 3, y() + 3, pw - 6, h() - 6, 4);
  }
  fl_color(p.fg_dim);
  fl_line_style(FL_SOLID, 2);
  int cxp = px + pw / 2, cyp = y() + h() / 2, r = std::min(6, h() / 4);
  fl_line(cxp - r, cyp, cxp + r, cyp);
  fl_line(cxp, cyp - r, cxp, cyp + r);
  fl_line_style(0);
  fl_pop_clip();
}

int TabBar::handle(int e) {
  switch (e) {
    case FL_ENTER:
      return 1;
    case FL_MOVE:
    case FL_LEAVE: {
      bool oc = false;
      int t = e == FL_LEAVE ? -1 : tab_at(Fl::event_x(), Fl::event_y(), oc);
      int px, pw;
      plus_rect(px, pw);
      bool hp = e != FL_LEAVE && Fl::event_x() >= px && Fl::event_x() < px + pw;
      if (t != hover_ || oc != hover_close_ || hp != hover_plus_) {
        hover_ = t;
        hover_close_ = oc;
        hover_plus_ = hp;
        redraw();
      }
      return 1;
    }
    case FL_PUSH: {
      bool oc = false;
      int t = tab_at(Fl::event_x(), Fl::event_y(), oc);
      int px, pw;
      plus_rect(px, pw);
      if (t < 0 && Fl::event_x() >= px && Fl::event_x() < px + pw) {
        if (on_new) on_new(cb_data);
        return 1;
      }
      if (t < 0) {
        // double click on the empty part of the bar: a new tab
        if (Fl::event_clicks() && Fl::event_button() == FL_LEFT_MOUSE && on_new) on_new(cb_data);
        return 1;
      }
      if (Fl::event_button() == FL_MIDDLE_MOUSE || (oc && Fl::event_button() == FL_LEFT_MOUSE)) {
        if (on_close) on_close(t, cb_data);
      } else if (Fl::event_button() == FL_LEFT_MOUSE && t != active_) {
        if (on_select) on_select(t, cb_data);
      }
      return 1;
    }
    case FL_MOUSEWHEEL: {
      int n = (int)labels_.size();
      if (n < 2) return 1;
      int d = Fl::event_dy() ? Fl::event_dy() : Fl::event_dx();
      int t = (active_ + (d > 0 ? 1 : -1) + n) % n;
      if (on_select) on_select(t, cb_data);
      return 1;
    }
  }
  return Fl_Widget::handle(e);
}
