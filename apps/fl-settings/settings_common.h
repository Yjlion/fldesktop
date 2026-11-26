// Helpers shared by the settings pages
#pragma once
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <functional>
#include <utility>
#include <vector>
#include <FL/Fl_Group.H>
#include <string>

#include "fld/ini.h"

namespace fld {

// fl-settings-helper (next to fl-settings in a build tree, else in libexec)
std::string settings_helper_path();

// Lays out "label: widget" rows top to bottom.
class Form {
public:
  Form(Fl_Group *g, int x, int y, int w, int label_w = 200);
  // a section heading
  Fl_Box *heading(const char *text);
  // a gray explanatory line
  Fl_Box *note(const char *text, int lines = 1);
  // returns the rectangle for a widget on a new row and adds the label
  void row(const char *label, int h, int &wx, int &wy, int &ww);
  void space(int h) { y_ += h; }
  int y() const { return y_; }
  int widget_x() const { return x_ + label_w_; }
  int widget_w() const { return w_ - label_w_; }
private:
  Fl_Group *g_;
  int x_, y_, w_, label_w_;
};

// Config files the pages edit. Each keeps comments when saved.
struct ConfigFile {
  std::string path;
  Ini ini;
  explicit ConfigFile(const std::string &p) : path(p) { ini.load(p); }
  void save() { ini.save(path); }
};

std::string wm_ini_path();
std::string desktop_ini_path();
std::string panel_ini_path();
// saves wm.ini and asks fl-wm to reload it
void save_wm(ConfigFile &f);

// The page background group with a title header; returns the content top y.
// The title is translated here (mark it with N_() or pass _()).
Fl_Group *page_group(int x, int y, int w, int h, const char *title, int &content_y);

// (section, key) pairs of a config file that a page sets
using ConfigKeys = std::vector<std::pair<std::string, std::string>>;
// Removes the keys (so the programs use their defaults) and saves; a wm.ini
// also makes fl-wm reload it.
void reset_keys(ConfigFile &f, const ConfigKeys &keys);
// A "Reset to Defaults" button: asks first, then reset_keys() and done()
// (which shows the default values on the page).
Fl_Button *reset_button(int x, int y, ConfigFile &f, ConfigKeys keys, std::function<void()> done);
// The same for keys in several files (path, keys); afterwards the page is
// made again from the files (reload_current_page()).
Fl_Button *reset_page_button(int x, int y, std::vector<std::pair<std::string, ConfigKeys>> files);
// Makes the page shown now again (main.cpp), e.g. after its settings changed.
void reload_current_page();

} // namespace fld
