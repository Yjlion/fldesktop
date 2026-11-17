#include "fld/glib_bridge.h"

#include <FL/Fl.H>
#include <glib.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace fld {

namespace {
GMainLoop *g_loop = nullptr;
GMainContext *g_ctx = nullptr;
// never destroyed: a program may exit() while the loop runs, and destroying
// a joinable std::thread at exit would call std::terminate
std::thread *g_thread = nullptr;

gboolean run_fn(gpointer data) {
  auto *fn = static_cast<std::function<void()> *>(data);
  (*fn)();
  delete fn;
  return G_SOURCE_REMOVE;
}

void awake_fn(void *data) {
  auto *fn = static_cast<std::function<void()> *>(data);
  (*fn)();
  delete fn;
}
} // namespace

void glib_start() {
  if (g_loop) return;
  // A context of our own, pushed as the worker's thread-default: GObject
  // libraries attach their sources to it. The global default context stays
  // with the FLTK thread, where GTK (libdecor's GTK plugin) iterates it.
  g_ctx = g_main_context_new();
  g_loop = g_main_loop_new(g_ctx, FALSE);
  std::mutex m;
  std::condition_variable cv;
  bool started = false;
  g_thread = new std::thread([&] {
    GMainLoop *loop = g_loop;
    g_main_context_push_thread_default(g_ctx);
    {
      std::lock_guard<std::mutex> l(m);
      started = true;
    }
    cv.notify_one();
    g_main_loop_run(loop);
    g_main_context_pop_thread_default(g_ctx);
  });
  std::unique_lock<std::mutex> l(m);
  cv.wait(l, [&] { return started; });
}

void glib_stop() {
  if (!g_loop) return;
  g_main_loop_quit(g_loop);
  g_main_context_wakeup(g_ctx);
  if (g_thread && g_thread->joinable()) g_thread->join();
  delete g_thread;
  g_thread = nullptr;
  g_main_loop_unref(g_loop);
  g_loop = nullptr;
  g_main_context_unref(g_ctx);
  g_ctx = nullptr;
}

bool glib_running() { return g_loop != nullptr; }

void glib_call(std::function<void()> fn) {
  // an idle source on our context always runs on the loop thread
  // (g_main_context_invoke might run it right here instead)
  GSource *src = g_idle_source_new();
  g_source_set_priority(src, G_PRIORITY_DEFAULT);
  g_source_set_callback(src, run_fn, new std::function<void()>(std::move(fn)), nullptr);
  g_source_attach(src, g_ctx);
  g_source_unref(src);
}

void glib_call_sync(std::function<void()> fn) {
  std::mutex m;
  std::condition_variable cv;
  bool done = false;
  glib_call([&] {
    fn();
    std::lock_guard<std::mutex> l(m);
    done = true;
    cv.notify_one();
  });
  std::unique_lock<std::mutex> l(m);
  cv.wait(l, [&] { return done; });
}

unsigned glib_timeout(unsigned ms, int (*fn)(void *), void *data) {
  GSource *src = g_timeout_source_new(ms);
  g_source_set_callback(src, (GSourceFunc)fn, data, nullptr);
  unsigned id = g_source_attach(src, g_ctx);
  g_source_unref(src);
  return id;
}

void ui_call(std::function<void()> fn) {
  Fl::awake(awake_fn, new std::function<void()>(std::move(fn)));
}

} // namespace fld
