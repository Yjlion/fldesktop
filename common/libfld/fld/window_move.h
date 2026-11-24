// Moving a window by dragging an empty part of its toolbar, menu bar or tab
// strip, as with a title bar (fl-wm's fld_window_manager_v1 protocol).
#pragma once
class Fl_Widget;
class Fl_Window;

namespace fld {

// Asks the compositor to move win's top-level window while the pressed
// button is held. False if it can't (X11, or another compositor).
bool begin_window_move(Fl_Window *win);

// For a widget's handle(): call it with the events its children did not
// take on an empty area. A left press starts tracking; a drag past a few
// pixels moves the window; a double click maximizes or restores it.
// Returns 1 if it used the event.
int window_drag_area(Fl_Widget *w, int event);

} // namespace fld

#include <FL/Fl_Menu_Bar.H>
namespace fld {
// A menu bar whose empty part (right of the last menu) moves the window.
class MenuBar : public Fl_Menu_Bar {
public:
  using Fl_Menu_Bar::Fl_Menu_Bar;
  int handle(int event) override;
  // x just past the last top-level menu title
  int items_end() const;
};
} // namespace fld
