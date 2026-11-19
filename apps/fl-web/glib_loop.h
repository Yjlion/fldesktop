// Runs GLib's default main context inside FLTK's event loop, on the main
// thread (WebKit's API must be used from the thread that owns the default
// context). The context's file descriptors and timeouts become FLTK fds and
// timeouts.
#pragma once

namespace web {

void glib_loop_start();
// Dispatches what is ready now (call after a burst of WebKit calls).
void glib_loop_poke();

} // namespace web
