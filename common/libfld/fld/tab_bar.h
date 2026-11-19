// A tab strip shared by fl-term, fl-edit and fl-web: tabs share the width,
// each with a close box, and a "+" button follows them. Tabs can be dragged
// to a new place (or out of the strip, to a window of their own), closed with
// the middle button, and have a right-click menu (the owner builds it).
#pragma once
#include <FL/Fl_Widget.H>

#include <functional>
#include <string>
#include <vector>

namespace fld {

struct TabInfo {
  std::string label;
  std::string tooltip;
  bool busy = false;     // a spinner before the label (a page loading)
  bool modified = false; // a dot before the label (unsaved changes)
};

class TabBar : public Fl_Widget {
public:
  TabBar(int x, int y, int w, int h);

  void set_tabs(const std::vector<TabInfo> &tabs, int active);
  void set_tabs(const std::vector<std::string> &labels, int active);
  int active() const { return active_; }
  int count() const { return (int)tabs_.size(); }

  std::string empty_label = "Untitled"; // shown for a tab without a label
  Fl_Color tint = 0;                     // the strip's color (0: from the theme)

  // user actions; the owner changes its tabs and calls set_tabs() again
  std::function<void(int)> on_select;
  std::function<void(int)> on_close;
  std::function<void()> on_new;
  std::function<void(int index)> on_menu;         // right click (-1: not on a tab)
  std::function<void(int from, int to)> on_move;  // dragged within the strip
  std::function<void(int index)> on_detach;       // dragged out of the strip (optional)

  void draw() override;
  int handle(int e) override;
  void resize(int x, int y, int w, int h) override;
  static int preferred_height();

  // where a tab dragged from `from` ends up, its left edge at drag_x
  // (relative to the first tab), with n tabs of width tw
  static int drop_index(int from, int drag_x, int tw, int n);

private:
  std::vector<TabInfo> tabs_;
  int active_ = 0;
  int hover_ = -1;
  bool hover_close_ = false, hover_plus_ = false;
  int scroll_ = 0; // first tab shown when they do not all fit
  // dragging
  int press_ = -1, press_x_ = 0, grab_dx_ = 0, drag_x_ = 0;
  bool dragging_ = false, outside_ = false;

  int arrows_w() const;
  int tab_width() const;
  int first_x() const;
  bool overflow() const;
  int visible_count() const;
  void tab_rect(int i, int &tx, int &tw) const;
  void close_rect(int i, int &cx, int &cy, int &cs) const;
  void plus_rect(int &px, int &pw) const;
  int tab_at(int ex, int ey, bool &on_close) const;
  void keep_visible(int i);
  void draw_tab(int i, int tx, int tw, Fl_Color bar, bool lifted);
};

// moves the element at from to index to (as TabBar::on_move reports it)
template <class T> void move_element(std::vector<T> &v, int from, int to) {
  if (from < 0 || to < 0 || from >= (int)v.size() || to >= (int)v.size() || from == to) return;
  T e = std::move(v[(size_t)from]);
  v.erase(v.begin() + from);
  v.insert(v.begin() + to, std::move(e));
}

// the index `cur` refers to after moving from -> to
int moved_index(int cur, int from, int to);

} // namespace fld
