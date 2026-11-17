// fl-bluetooth: Bluetooth settings for fldesktop (BlueZ)
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include <cstdio>
#include <string>

#include "bt_ui.h"
#include "btclient.h"
#include "fld/theme.h"

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-bluetooth\nTurns Bluetooth on and off, pairs and connects devices.\n");
      return 0;
    }
  }
  fld::init_app("fl-bluetooth", argc, argv);
  Fl::lock();
  Fl_Double_Window *win = new Fl_Double_Window(640, 560, "Bluetooth");
  bt::BluetoothView *view = new bt::BluetoothView(16, 12, 608, 536);
  win->resizable(view);
  win->end();
  win->size_range(520, 420);
  fld::set_window_icon(win, "fl-bluetooth");
  bt::Client::get().on_changed = [view] { view->refresh(); };
  // pairing works with the window open even without the panel's agent
  bt::Client::get().start_agent(bt::ask_agent);
  win->show(); // not show(argc, argv): it resets the theme colors
  return Fl::run();
}
