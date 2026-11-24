// Windows open at the size they had last time
#pragma once
#include <string>

class Fl_Window;

namespace fld {

// Gives `win` (before show()) the size it had when the program last ran,
// from ~/.local/state/fldesktop/windows.ini, and remembers its size from now
// on. The first time, a window taller than 85 % of the work area is made
// smaller, so what is behind it stays in view. Sizes are kept within the
// work area and the window's size_range().
void remember_window_size(Fl_Window *win, const std::string &app);

// (exposed for tests) the size to open at: the saved one if any, else the
// default capped at 85 % of the area's height; within min_w/min_h and the area
void window_open_size(int saved_w, int saved_h, int def_w, int def_h, int area_w, int area_h, int min_w,
                      int min_h, int &w, int &h);

} // namespace fld
