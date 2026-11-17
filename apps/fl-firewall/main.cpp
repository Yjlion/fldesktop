// fl-firewall: firewalld settings for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>

#include <cstdio>
#include <string>

#include "fld/notify.h"
#include "fld/theme.h"
#include "fw_ui.h"
#include "fwclient.h"

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-firewall\nConfigures the firewalld zones, services, ports and forwarding.\n");
      return 0;
    }
  }
  fld::init_app("fl-firewall", argc, argv);
  Fl::lock();
  Fl_Double_Window *win = new Fl_Double_Window(900, 620, "Firewall");
  fw::FirewallView *view = new fw::FirewallView(12, 12, 876, 596);
  win->resizable(view);
  win->end();
  win->size_range(760, 480);
  fld::set_window_icon(win, "fl-firewall");
  static bool was_panic = false, known = false;
  fw::Client::get().on_changed = [view] {
    view->refresh();
    const fw::State &st = fw::Client::get().state();
    if (known && st.running && st.panic && !was_panic) {
      fld::Notification n;
      n.summary = "Panic mode is on";
      n.body = "The firewall blocks all network traffic until panic mode is turned off.";
      n.icon = "fl-firewall";
      n.urgency = 2;
      fld::notify(n);
    }
    was_panic = st.panic;
    known = st.running;
  };
  win->show(); // not show(argc, argv): it resets the theme colors
  return Fl::run();
}
