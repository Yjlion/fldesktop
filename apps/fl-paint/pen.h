// fl-paint: pen pressure through the Wayland tablet protocol (tablet-v2).
// With it bound the compositor no longer turns the pen into a pointer for
// this program, so pen_start() makes the pen's tip, motion and lift into the
// same FLTK push, drag and release events a mouse gives, and keeps the
// pressure for the brush. Off unless [Paint] PenPressure=true.
#pragma once

class Fl_Window;

namespace pen {

// binds the tablet manager when the compositor has one; false otherwise
bool start(Fl_Window *win);
bool active();      // a pen is touching the canvas now
double pressure();  // 0..1 (1 without a pen)

}  // namespace pen
