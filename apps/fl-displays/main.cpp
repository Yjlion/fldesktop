// fl-displays: resolution, scale, rotation and position of the screens.
// It only needs wlr-output-management, so it works on fl-wm, sway, labwc,
// Hyprland and other wlroots compositors. Under fl-wm the arrangement is
// kept in wm.ini; elsewhere it lasts until the compositor restarts.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/platform.H>
#include <cstdio>
#include <cstring>
#include <string>

#include "displays_ui.h"
#include "fld/i18n.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "outputs.h"

static bool under_flwm() {
  const char *d = getenv("XDG_CURRENT_DESKTOP");
  return getenv("FLWM_PID") || (d && strstr(d, "fldesktop"));
}

int main(int argc, char **argv) {
  fld::i18n_init();
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("%s", _("Usage: fl-displays\n"
                     "Sets the resolution, scale, rotation and position of the screens\n"
                     "(any compositor with wlr-output-management).\n"));
      return 0;
    }
  }
  fld::init_app("fl-displays", argc, argv);
  const int W = 720, PAD = 16;
  Fl_Double_Window *win = new Fl_Double_Window(W, fld::DisplaysView::kHeight + 2 * PAD + 30, _("Displays"));
  win->color(fld::palette().bg2);
  bool ok = fld::outputs::connect(fl_wl_display());
  fld::DisplaysView *view = new fld::DisplaysView(PAD, PAD, W - 2 * PAD);
  Fl_Box *note = new Fl_Box(PAD, PAD + fld::DisplaysView::kHeight + 4, W - 2 * PAD, 26);
  note->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  note->labelsize(12);
  note->labelcolor(fld::palette().fg_dim);
  if (!fl_wl_display())
    note->label(_("fl-displays needs a Wayland session."));
  else if (!ok)
    note->label(_("This compositor does not let programs change the screens (wlr-output-management)."));
  else if (!under_flwm())
    note->label(_("The arrangement lasts until the compositor restarts: keep it in its own configuration."));
  win->end();
  if (under_flwm())
    view->on_saved = [](const std::vector<fld::outputs::HeadConfig> &cfg) {
      std::string path = fld::fld_config_dir() + "/wm.ini";
      fld::Ini wm;
      wm.load(path);
      fld::outputs::write_wm_config(wm, cfg);
      wm.save(path);  // fl-wm already runs with this configuration
    };
  auto fit = [win, view, note](int) {
    note->position(note->x(), view->y() + view->h() + 4);
    win->size(win->w(), note->y() + note->h() + 16);
  };
  view->on_resize = fit;
  fit(0);
  fld::outputs::on_changed = [view] { view->refresh(); };
  fld::set_window_icon(win, "fl-displays");
  win->show();  // not show(argc, argv): it resets the theme colors
  return Fl::run();
}
