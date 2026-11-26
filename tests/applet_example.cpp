// An example fl-panel applet plugin (fld_applet.h): a button showing how
// long the computer has been up; a click opens the task manager. Built as a
// module for test_applets; doc/APPLETS.md walks through it.
//   c++ -shared -fPIC $(fltk-config --cxxflags) applet_example.cpp
//       $(fltk-config --ldflags) -o uptime.so      (one line)
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <cstdio>
#include <string>

#include "fld_applet.h"

#ifndef EXAMPLE_ABI
#define EXAMPLE_ABI FLD_APPLET_ABI_VERSION
#endif

namespace {

const fld_applet_host *g_host;

class UptimeButton : public Fl_Button {
public:
  std::string text;
  UptimeButton(int h) : Fl_Button(0, 0, 80, h) {
    box(FL_FLAT_BOX);
    color(g_host->bg);
    labelcolor(g_host->fg);
    callback([](Fl_Widget *, void *) {
      const char *argv[] = {"fl-taskmgr", nullptr};
      g_host->spawn(argv);
    });
    update();
    Fl::add_timeout(60, tick, this);
  }
  ~UptimeButton() override { Fl::remove_timeout(tick, this); }
  static void tick(void *d) {
    ((UptimeButton *)d)->update();
    Fl::repeat_timeout(60, tick, d);
  }
  void update() {
    double up = 0;
    if (FILE *f = fopen("/proc/uptime", "r")) {
      if (fscanf(f, "%lf", &up) != 1) up = 0;
      fclose(f);
    }
    char buf[32];
    snprintf(buf, sizeof buf, "%s %dh %02dm", g_host->config("uptime", "Prefix", "up"), (int)up / 3600,
             (int)up / 60 % 60);
    text = buf;
    label(text.c_str());
    redraw();
  }
};

void *create(const fld_applet_host *host) {
  g_host = host;
  return static_cast<Fl_Widget *>(new UptimeButton(host->height));
}

void destroy(void *w) { delete static_cast<Fl_Widget *>(w); }

const fld_applet kApplet = {EXAMPLE_ABI, "uptime", "Uptime", "How long the computer has been up", create, destroy};

}  // namespace

#ifndef EXAMPLE_NO_ENTRY  // (the test builds a copy without the entry point)
extern "C" __attribute__((visibility("default"))) const fld_applet *fld_applet_v1(void) { return &kApplet; }
#else
extern "C" const fld_applet *not_the_entry(void) { return &kApplet; }
#endif
