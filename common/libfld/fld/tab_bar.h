// A tab strip shared by fl-term, fl-edit, fl-web and fl-files: tabs share the
// width, each with a close box, and a "+" button follows them. Tabs can be
// dragged to a new place, out of the strip (into a window of their own) or
// onto another window of the same program (see fld/tab_move.h), closed with
// the middle button, and have a right-click menu (the owner builds it).
#pragma once
#include <FL/Fl_Widget.H>

class Fl_Window;

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
  ~TabBar() override;

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

  // Moving tabs between windows (all optional; without on_export a tab
  // dragged out of the strip goes to on_detach). Tabs move between strips
  // with the same app_id, also in other processes.
  std::string app_id;
  std::function<std::string(int index)> on_export; // the tab's state ("": it cannot move)
  // a tab dropped here at index `at`; same_process: it comes from this
  // program's own window. Return false to refuse it.
  std::function<bool(const std::string &state, int at, bool same_process)> on_import;
  std::function<void(int index)> on_moved_away; // another window took it: close it, no questions
  // dropped where no window took it: open a window with this state file
  // (e.g. spawn "<app> --adopt-tab <file>"); without it on_detach runs
  std::function<void(int index, const std::string &file)> on_tear_off;

  // the strip in window w that takes tabs of app (nullptr: none)
  static TabBar *in_window(Fl_Window *w, const std::string &app = "");
  // where a tab dropped at ex (window coordinates) is inserted; count() past the end
  int insert_index(int ex) const;
  bool over(int ey) const { return visible_r() && ey >= y() - 8 && ey < y() + h() + 8; }
  // the dropped payload handling (the tab drop hook calls it)
  bool take_drop(const std::string &file, int ex, int ey);

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
  int out_ = -1;        // the tab being dragged to another window (dimmed)
  int dnd_insert_ = -1; // a tab dragged over this strip lands here
  bool start_tab_drag(int i);

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
