// The primary selection: text selected with the mouse, pasted with the
// middle button. FLTK keeps it inside the program on Wayland, so this
// speaks zwp_primary_selection_v1 to share it with other programs. On X11
// (or a compositor without the protocol) it falls back to FLTK's own
// Fl::copy(..., 0) / Fl::paste(..., 0).
#pragma once
#include <string>

class Fl_Widget;

namespace fld {

// Makes `text` the primary selection (empty text does nothing).
void primary_copy(const std::string &text);
// Pastes the primary selection into `w`: it gets FL_PASTE with
// Fl::event_text(), right away or once the other program has sent it.
void primary_paste(Fl_Widget &w);
// Whether the Wayland protocol is in use (for tests and diagnostics).
bool primary_native();

} // namespace fld
