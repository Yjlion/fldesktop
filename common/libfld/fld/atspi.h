// An AT-SPI2 bridge: makes the windows and widgets of an FLTK program
// visible to screen readers (Orca) and other assistive technologies.
//
// fld::init_app() starts it when the accessibility bus says assistive
// technologies are in use (org.a11y.Status IsEnabled or
// ScreenReaderEnabled), or when FLD_ATSPI=1; FLD_ATSPI=0 keeps it off.
// AT_SPI_BUS_ADDRESS, when set, is the bus to use.
//
// The widget tree is read on the FLTK thread a few times a second into a
// snapshot; a worker thread answers D-Bus calls from it. Actions (a click,
// new text, a value) are carried out on the FLTK thread.
#pragma once
#include <string>

class Fl_Widget;

namespace fld {

void atspi_start(const std::string &app_name);
bool atspi_running();
// Programs with their own widgets can name them: an AT-SPI role number
// (atspi_model.h) and a name; role 0 keeps the guessed role.
void atspi_describe(Fl_Widget *w, unsigned role, const std::string &name);

} // namespace fld
