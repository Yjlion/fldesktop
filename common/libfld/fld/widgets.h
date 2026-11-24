// Small shared widgets
#pragma once
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Group.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Round_Button.H>
#include <FL/Fl_Tabs.H>
#include <string>

namespace fld {

// Flat button with an optional theme icon, highlighted on hover.
class FlatButton : public Fl_Button {
public:
  FlatButton(int x, int y, int w, int h, const char *label = nullptr);
  void icon_name(const char *name, int size = 20);
  // sets the icon image (not owned; e.g. from fld::icon or fld::symbolic_icon)
  void icon_image(Fl_Image *img) { icon_ = img; redraw(); }
  void hover_color(Fl_Color c) { hover_col_ = c; }
  void text_color(Fl_Color c) { labelcolor(c); }
  void active_look(bool on) { active_look_ = on; redraw(); }
  bool active_look() const { return active_look_; }
  int handle(int e) override;
protected:
  void draw() override;
private:
  bool hover_ = false, active_look_ = false;
  Fl_Color hover_c() const;
  Fl_Color hover_col_ = 0;
  Fl_Image *icon_ = nullptr;
};

// Horizontal toolbar: add FlatButtons with add_button(); separators with add_separator().
class Toolbar : public Fl_Group {
public:
  Toolbar(int x, int y, int w, int h);
  FlatButton *add_button(const char *icon, const char *tooltip, Fl_Callback *cb, void *data,
                         const char *label = nullptr);
  void add_separator();
  // Adds an arbitrary widget of width w at the current position.
  void add_widget(Fl_Widget *w);
  int next_x() const { return next_x_; }
  int handle(int event) override; // empty parts move the window
protected:
  void draw() override;
private:
  int next_x_;
};

// The flat look of Files and Settings for the stock FLTK widgets, in every
// program: tabs underlined in the accent, check boxes and radio buttons
// filled with it, and keyboard focus as a 2 px accent ring (instead of
// FLTK's dotted box).
class Tabs : public Fl_Tabs {
public:
  Tabs(int x, int y, int w, int h, const char *label = nullptr);
  int handle(int event) override; // the strip right of the last tab moves the window
protected:
  void draw() override;
  void draw_tab(int x1, int x2, int W, int H, Fl_Widget *o, int flags, int what) override;
};

class CheckButton : public Fl_Check_Button {
public:
  CheckButton(int x, int y, int w, int h, const char *label = nullptr) : Fl_Check_Button(x, y, w, h, label) {}
protected:
  void draw() override;
};

class RadioButton : public Fl_Round_Button {
public:
  RadioButton(int x, int y, int w, int h, const char *label = nullptr) : Fl_Round_Button(x, y, w, h, label) {}
protected:
  void draw() override;
};

// A text field for searching or filtering: a magnifier and, while it is
// empty, a dim placeholder ("Search applications").
class SearchInput : public Fl_Input {
public:
  SearchInput(int x, int y, int w, int h, const char *placeholder = nullptr);
  void placeholder(const char *text) { placeholder_ = text ? text : ""; redraw(); }
protected:
  void draw() override;
private:
  std::string placeholder_;
};

// Draws the keyboard focus ring: 2 px of the accent around x, y, w, h.
void draw_focus_ring(int x, int y, int w, int h, int radius = 4);

// Escapes '@' so FLTK does not treat it as a symbol prefix in labels/fl_draw boxes.
std::string escape_at(const std::string &s);

void about_dialog(const char *app_name, const char *icon, const char *description);

} // namespace fld
