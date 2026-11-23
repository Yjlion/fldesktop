// Keeps the screen on while a video plays (zwp_idle_inhibit_manager_v1).
#pragma once
class Fl_Window;
namespace idle {
// Inhibits idle for the window's surface (on = true) or stops (on = false).
// Does nothing when the compositor lacks the protocol or not on Wayland.
void inhibit(Fl_Window *win, bool on);
}
