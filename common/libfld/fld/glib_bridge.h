// Runs the GLib default main context in a worker thread so GObject based
// libraries (libnm, polkit-agent) can be used from FLTK programs.
//
//  - glib_call(fn): run fn on the GLib thread (all GObject code goes there)
//  - ui_call(fn):   run fn on the FLTK thread (needs Fl::lock() in main())
#pragma once
#include <functional>

namespace fld {

void glib_start();
void glib_stop();
bool glib_running();
void glib_call(std::function<void()> fn);
void ui_call(std::function<void()> fn);
// Runs fn on the GLib thread and waits for it to finish.
void glib_call_sync(std::function<void()> fn);

} // namespace fld
