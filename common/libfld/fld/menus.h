// Popup menus: a builder for right-click menus, and the Cut/Copy/Paste menu
// every text field and text editor gets (installed by fld::init_app).
#pragma once
#include <FL/Fl_Menu_Item.H>

#include <deque>
#include <functional>
#include <string>
#include <vector>

class Fl_Widget;

namespace fld {

// Builds an Fl_Menu_Item list; each item has a numeric id (never 0) or an
// action. popup() shows it and returns the chosen id (0: none), after running
// the item's action if it has one.
class Menu {
public:
  // label is a menu label ('&' underlines, '@' is a symbol): use text() for
  // labels made from file names, titles and other outside text
  Menu &item(const std::string &label, long id, int flags = 0, int shortcut = 0);
  Menu &item(const std::string &label, std::function<void()> action, int flags = 0, int shortcut = 0);
  Menu &toggle(const std::string &label, bool on, std::function<void()> action, int flags = 0);
  Menu &radio(const std::string &label, bool on, std::function<void()> action, int flags = 0);
  // an item that does what button b does (its label unless one is given);
  // left out while b is hidden, greyed out while it is inactive; a check
  // button becomes a toggle
  Menu &button(Fl_Widget *b, const std::string &label = "");
  Menu &divider(); // after the last item
  Menu &submenu(const std::string &label, int flags = 0);
  Menu &end_submenu();
  bool empty() const { return items_.empty(); }

  long popup(int x, int y, const char *title = nullptr); // at x, y in the current window
  long popup();                                          // at the pointer
  const Fl_Menu_Item *items();                           // the terminated list
  void run(long id);                                     // runs the action of the item with this id

  static std::string text(const std::string &s); // escapes '&' and '@'

private:
  struct Entry {
    std::string label;
    long id = 0;
    int flags = 0, shortcut = 0, action = -1;
    bool end = false;
  };
  std::vector<Entry> items_;
  std::vector<std::function<void()>> actions_;
  std::vector<Fl_Menu_Item> built_;
  std::deque<std::string> labels_;
};

// A right-click menu for any widget (a list, a table, a canvas): fill adds
// the items when it opens. With select_first the click goes to the widget
// first as an ordinary click, so a list or table selects the row under the
// pointer and fill sees it selected. Widgets inside w get the menu too unless
// they have one of their own. Needs install_edit_menu (fld::init_app does it).
void set_context_menu(Fl_Widget *w, std::function<void(Menu &)> fill, bool select_first = true);
void remove_context_menu(Fl_Widget *w);

// The text field menu (Undo, Redo, Cut, Copy, Paste, Delete, Select All) for
// Fl_Input_ and Fl_Text_Display widgets. init_app installs it; widgets with a
// menu of their own opt out.
void install_edit_menu();
void no_edit_menu(Fl_Widget *w);
// shows the menu for w (an Fl_Input_ or Fl_Text_Display) at the pointer
bool show_edit_menu(Fl_Widget *w);

} // namespace fld
