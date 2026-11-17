// fl-term tab bar: one tab per terminal, a close box on each and a "+" button
#pragma once
#include <FL/Fl_Widget.H>
#include <string>
#include <vector>

class TabBar : public Fl_Widget {
public:
  TabBar(int x, int y, int w, int h);

  void set_tabs(const std::vector<std::string> &labels, int active);
  int active() const { return active_; }
  int count() const { return (int)labels_.size(); }

  // user actions; the owner updates the tabs afterwards
  void (*on_select)(int index, void *) = nullptr;
  void (*on_close)(int index, void *) = nullptr;
  void (*on_new)(void *) = nullptr;
  void *cb_data = nullptr;

  void draw() override;
  int handle(int e) override;
  static int preferred_height();

private:
  std::vector<std::string> labels_;
  int active_ = 0;
  int hover_ = -1;          // tab under the pointer
  bool hover_close_ = false; // ... on its close box
  bool hover_plus_ = false;

  int tab_width() const;
  void tab_rect(int i, int &tx, int &tw) const;
  void close_rect(int i, int &cx, int &cy, int &cs) const;
  void plus_rect(int &px, int &pw) const;
  // index of the tab at ex (-1 none); sets on_close when over its close box
  int tab_at(int ex, int ey, bool &on_close) const;
};
