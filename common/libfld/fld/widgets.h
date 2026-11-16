// Small shared widgets
#pragma once
#include <FL/Fl_Button.H>
#include <FL/Fl_Group.H>
#include <string>

namespace fld {

// Flat button with an optional theme icon, highlighted on hover.
class FlatButton : public Fl_Button {
public:
  FlatButton(int x, int y, int w, int h, const char *label = nullptr);
  void icon_name(const char *name, int size = 20);
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
protected:
  void draw() override;
private:
  int next_x_;
};

void about_dialog(const char *app_name, const char *icon, const char *description);

} // namespace fld
