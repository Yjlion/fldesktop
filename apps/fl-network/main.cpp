// fl-network: NetworkManager preferences for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include <string>

#include "fld/theme.h"
#include "network_ui.h"
#include "nmclient.h"

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-network\nShows and edits NetworkManager devices and connections.\n");
      return 0;
    }
  }
  fld::init_app("fl-network", argc, argv);
  Fl::lock();
  nm::Client::get().start();
  Fl_Double_Window *win = new Fl_Double_Window(860, 560, "Network");
  nm::NetworkView *view = new nm::NetworkView(0, 0, 860, 560);
  win->resizable(view);
  win->end();
  win->size_range(640, 400);
  fld::set_window_icon(win, "fl-network");
  nm::Client::get().on_changed = [view] { view->refresh(); };
  win->show(argc, argv);
  return Fl::run();
}
