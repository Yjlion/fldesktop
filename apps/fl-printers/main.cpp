// fl-printers: CUPS printer settings for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include <cstdio>
#include <string>

#include "fld/theme.h"
#include "printers_ui.h"

int main(int argc, char **argv) {
  std::string select;
  bool add = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-printers [--add] [PRINTER]\nShows, adds and configures CUPS printers and their jobs.\n");
      return 0;
    } else if (a == "--add") {
      add = true;
    } else {
      select = a;
    }
  }
  fld::init_app("fl-printers", argc, argv);
  Fl::lock();
  Fl_Double_Window *win = new Fl_Double_Window(900, 600, "Printers");
  prn::PrintersView *view = new prn::PrintersView(12, 12, 876, 576);
  win->resizable(view);
  win->end();
  win->size_range(720, 480);
  fld::set_window_icon(win, "fl-printers");
  if (!select.empty()) view->select(select);
  win->show(); // not show(argc, argv): it resets the theme colors
  if (add) {
    std::string n = prn::add_printer_dialog();
    if (!n.empty()) {
      view->refresh();
      view->select(n);
    }
  }
  return Fl::run();
}
