#include "fld/glib_bridge.h"

#include <FL/Fl.H>
#include <glib.h>

#include <condition_variable>
#include <mutex>
#include <thread>

namespace fld {

namespace {
GMainLoop *g_loop = nullptr;
std::thread g_thread;

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
  g_loop = g_main_loop_new(nullptr, FALSE);
  std::mutex m;
  std::condition_variable cv;
  bool started = false;
  g_thread = std::thread([&] {
    GMainLoop *loop = g_loop;
    g_main_context_acquire(g_main_context_default());
    {
      std::lock_guard<std::mutex> l(m);
      started = true;
    }
    cv.notify_one();
    g_main_loop_run(loop);
    g_main_context_release(g_main_context_default());
  });
  std::unique_lock<std::mutex> l(m);
  cv.wait(l, [&] { return started; });
}

void glib_stop() {
  if (!g_loop) return;
  g_main_loop_quit(g_loop);
  g_main_context_wakeup(g_main_context_default());
  if (g_thread.joinable()) g_thread.join();
  g_main_loop_unref(g_loop);
  g_loop = nullptr;
}

bool glib_running() { return g_loop != nullptr; }

void glib_call(std::function<void()> fn) {
  // an idle source on the default context always runs on the loop thread
  // (g_main_context_invoke might run it right here instead)
  g_idle_add_full(G_PRIORITY_DEFAULT, run_fn, new std::function<void()>(std::move(fn)), nullptr);
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

void ui_call(std::function<void()> fn) {
  Fl::awake(awake_fn, new std::function<void()>(std::move(fn)));
}

} // namespace fld
