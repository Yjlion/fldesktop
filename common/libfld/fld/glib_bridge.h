// Runs a GLib main context in a worker thread so GObject based libraries
// (libnm, polkit-agent, GDBus) can be used from FLTK programs. The context is
// the worker's thread-default context, not the global default one (which
// FLTK's GTK based window decorations use), so code on the GLib thread must
// attach sources with g_main_context_get_thread_default() / glib_timeout().
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
// g_timeout_add on the GLib thread's context; call it on the GLib thread.
unsigned glib_timeout(unsigned ms, int (*fn)(void *), void *data);

} // namespace fld
