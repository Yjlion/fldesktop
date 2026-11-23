// A window with one widget of each kind, for tests/atspi-smoke.sh
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Hor_Slider.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Output.H>

#include "fld/theme.h"

int main(int argc, char **argv) {
  fld::init_app("atspi-app", argc, argv);
  Fl_Double_Window w(400, 320, "A11y Test");
  Fl_Button press(20, 20, 120, 30, "Press me");
  Fl_Check_Button wrap(160, 20, 150, 30, "Wrap lines");
  Fl_Input name(90, 70, 200, 30, "Name:");
  Fl_Output result(90, 110, 200, 30, "Result:");
  Fl_Hor_Slider vol(90, 150, 200, 24);
  vol.tooltip("Volume");
  vol.range(0, 100);
  vol.step(1);
  Fl_Hold_Browser list(90, 190, 200, 100);
  list.add("Apple");
  list.add("@bBanana");
  press.callback([](Fl_Widget *, void *r) { ((Fl_Output *)r)->value("pressed"); }, &result);
  w.end();
  w.show();
  Fl::add_timeout(60, [](void *) { exit(0); });
  return Fl::run();
}
