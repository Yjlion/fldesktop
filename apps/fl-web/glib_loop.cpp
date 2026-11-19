#include "glib_loop.h"

#include <FL/Fl.H>
#include <glib.h>
#include <poll.h>

#include <vector>

namespace web {
namespace {

std::vector<GPollFD> g_fds;
std::vector<std::pair<int, int>> g_registered; // fd, FLTK flags
bool g_busy = false;

void fire(FL_SOCKET, void *);
void fire_timeout(void *) { fire(-1, nullptr); }

void unregister() {
  for (auto &r : g_registered) Fl::remove_fd(r.first, r.second);
  g_registered.clear();
  Fl::remove_timeout(fire_timeout);
}

// one prepare/query/check cycle without blocking; registers what to wait for
void arm() {
  GMainContext *ctx = g_main_context_default();
  if (!g_main_context_acquire(ctx)) {
    Fl::add_timeout(0.01, fire_timeout); // owned elsewhere: look again soon
    return;
  }
  gint prio = 0, timeout = -1;
  gboolean ready = g_main_context_prepare(ctx, &prio);
  int n;
  while ((n = g_main_context_query(ctx, prio, &timeout, g_fds.data(), (gint)g_fds.size())) > (int)g_fds.size())
    g_fds.resize((size_t)n);
  if (n > 0) g_poll(g_fds.data(), (guint)n, 0);
  if (g_main_context_check(ctx, prio, g_fds.data(), n) || ready) {
    g_main_context_dispatch(ctx);
    timeout = 0; // more may be ready
  }
  g_main_context_release(ctx);
  for (int i = 0; i < n; i++) {
    int flags = 0;
    if (g_fds[(size_t)i].events & (G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_PRI)) flags |= FL_READ;
    if (g_fds[(size_t)i].events & G_IO_OUT) flags |= FL_WRITE;
    if (!flags) continue;
    Fl::add_fd(g_fds[(size_t)i].fd, flags, fire);
    g_registered.push_back({g_fds[(size_t)i].fd, flags});
  }
  if (timeout >= 0) Fl::add_timeout(timeout / 1000.0, fire_timeout);
}

void fire(FL_SOCKET, void *) {
  // dialogs run nested FLTK loops: GLib waits until the outer dispatch is done
  if (g_busy) return;
  g_busy = true;
  unregister();
  GMainContext *ctx = g_main_context_default();
  for (int i = 0; i < 16 && g_main_context_iteration(ctx, FALSE); i++) {}
  arm();
  g_busy = false;
}

} // namespace

void glib_loop_start() {
  g_fds.resize(16);
  g_busy = true;
  arm();
  g_busy = false;
}

void glib_loop_poke() {
  Fl::remove_timeout(fire_timeout);
  Fl::add_timeout(0, fire_timeout);
}

} // namespace web
