// glib_call / glib_call_sync run on the GLib thread, ui_call in the FLTK thread
#include <FL/Fl.H>
#include <glib.h>
#include <thread>

#include "fld/glib_bridge.h"
#include "test.h"

int main() {
  Fl::lock();
  fld::glib_start();
  std::thread::id main_id = std::this_thread::get_id(), glib_id;
  bool owner = false;
  fld::glib_call_sync([&] {
    glib_id = std::this_thread::get_id();
    owner = g_main_context_is_owner(g_main_context_default());
  });
  CHECK(glib_id != main_id);
  CHECK(owner);
  int ui_runs = 0;
  std::thread::id ui_id;
  fld::glib_call([&] {
    fld::ui_call([&] { ui_runs++; ui_id = std::this_thread::get_id(); });
  });
  for (int i = 0; i < 200 && !ui_runs; i++) Fl::wait(0.01);
  CHECK_EQ(ui_runs, 1);
  CHECK(ui_id == main_id);
  fld::glib_stop();
  CHECK(!fld::glib_running());
  return test_result("glib");
}
