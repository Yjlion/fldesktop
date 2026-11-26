# Panel applet plugins

fl-panel loads applets from shared libraries, so an applet can be written and
installed without rebuilding the panel. The interface is the C header
[`shell/fld_applet.h`](../shell/fld_applet.h), installed as
`/usr/include/fldesktop/fld_applet.h`. The example is
[`tests/applet_example.cpp`](../tests/applet_example.cpp): a button that shows the
uptime and opens the task manager.

## Writing one

```c++
#include <FL/Fl_Button.H>
#include <fldesktop/fld_applet.h>

static void *create(const fld_applet_host *host) {
  // the panel's window is the current group: just make the widget
  Fl_Button *b = new Fl_Button(0, 0, 80, host->height, "Hello");
  b->box(FL_FLAT_BOX);
  b->color(host->bg);
  b->labelcolor(host->fg);
  return static_cast<Fl_Widget *>(b);
}

static const fld_applet applet = {FLD_APPLET_ABI_VERSION, "hello", "Hello", "Says hello", create, nullptr};

extern "C" __attribute__((visibility("default"))) const fld_applet *fld_applet_v1(void) { return &applet; }
```

Build and install it:

```sh
c++ -shared -fPIC $(fltk-config --cxxflags) hello.cpp $(fltk-config --ldflags) -o hello.so
install -D hello.so ~/.local/lib/fldesktop/applets/hello.so
```

Then restart the panel (`pkill -HUP fl-panel`, or log out and back in).

## The rules

- **Where.** The panel looks in each directory of `$FLD_APPLET_PATH` (colon
  separated), then in `~/.local/lib/fldesktop/applets`, then in
  `<libdir>/fldesktop/applets`. When two directories have the same applet id,
  the first one wins, so a user's copy replaces the system one.
- **The widget.** `create()` makes exactly one widget, `host->height` high. The
  panel decides its position and keeps its width. Hide it with `hide()` when
  there is nothing to show, then call `host->relayout()`. Do the same after a
  resize.
- **Threads.** Use the FLTK loop (`Fl::add_timeout`, `Fl::add_fd`). For work in a
  thread, use `Fl::awake()` to get back to it. The panel has called `Fl::lock()`.
- **Settings.** `host->config(id, key, def)` reads `[Applet:<id>]` in
  `~/.config/fldesktop/panel.ini`.
- **Turning one off.** Add its id to `[Panel] DisabledApplets=` in `panel.ini`.
- **Compatibility.** The ABI is the two structs in the header and the
  `fld_applet_v1` entry point. `FLD_APPLET_ABI_VERSION` only changes when they
  change. A plugin built for another version is not loaded, and the panel
  prints why on its standard error. Applets are C++ code linked against FLTK,
  so build them with the same FLTK 1.4 the panel uses.
- **Trust.** A plugin runs inside the panel with your rights, like any other
  program you install. The panel loads only from the directories above.

The built-in applets (weather, system monitor, privacy indicator and the
others) are compiled into the panel and use its internals. New applets that
do not need those internals can be plugins.
