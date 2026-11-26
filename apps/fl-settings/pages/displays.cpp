// Displays: resolution, scale, rotation and position through wlr-output-management
// settings-index: also common/displays/displays_ui.cpp
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <cstdlib>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#ifdef FLD_HAVE_BACKLIGHT
#include "backlight.h"
#endif
#include <FL/Fl_Slider.H>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "displays_ui.h"
#include "outputs.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

// a rotation sensor (iio-sensor-proxy reads it): convertibles and tablets
bool has_accelerometer() {
  for (const std::string &d : list_dir("/sys/bus/iio/devices"))
    if (file_exists("/sys/bus/iio/devices/" + d + "/in_accel_x_raw") ||
        file_exists("/sys/bus/iio/devices/" + d + "/in_accel_x_raw0"))
      return true;
  return false;
}

// The arrangement editor (common/displays, shared with fl-displays) and,
// below it, what only fldesktop has: rotation with the device, the
// built-in screen's brightness, pointers to Night Light and Advanced.
class DisplaysPage : public Fl_Group {
public:
  DisplaysView *view;

  DisplaysPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Displays"), cy);
    resizable(g);
    int lw = std::min(W - 48, 680);
    outputs::connect(fl_wl_display());
    view = new DisplaysView(X + 24, cy, lw);
    view->on_saved = [](const std::vector<outputs::HeadConfig> &cfg) {
      ConfigFile wm(wm_ini_path());
      outputs::write_wm_config(wm.ini, cfg);
      wm.save();  // fl-wm already runs with this configuration
    };
    int ny = view->y() + DisplaysView::kHeight + 18;  // moved up below when there is one screen
    if (has_accelerometer()) {
      // convertibles and tablets: fl-autorotate follows the accelerometer
      Fl_Check_Button *rot = new fld::CheckButton(X + 24, ny, lw, 26, _("Rotate the built-in screen with the device"));
      rot->tooltip(_("For convertibles and tablets with a rotation sensor (iio-sensor-proxy)"));
      ConfigFile d(desktop_ini_path());
      rot->value(d.ini.get_bool("Display", "AutoRotate", true));
      rot->callback([](Fl_Widget *w, void *) {
        ConfigFile d(desktop_ini_path());
        d.ini.set_bool("Display", "AutoRotate", ((Fl_Check_Button *)w)->value());
        d.save();
      });
      ny += 34;
    }
    int wx, wy, ww;
#ifdef FLD_HAVE_BACKLIGHT
    // the built-in screen's backlight (laptops)
    int pct;
    if (bl::screen_get(pct)) {
      Form bf(g, X + 24, ny, lw);
      bf.heading(_("Brightness"));
      bf.row(_("Built-in screen:"), 26, wx, wy, ww);
      Fl_Slider *b = new Fl_Slider(wx, wy, std::min(ww, 320), 26);
      b->type(FL_HOR_NICE_SLIDER);
      b->bounds(1, 100);
      b->step(1);
      b->value(pct);
      b->when(FL_WHEN_CHANGED);
      b->callback([](Fl_Widget *w, void *) {
        std::string err;
        if (!bl::screen_set((int)((Fl_Slider *)w)->value(), err)) fprintf(stderr, "fl-settings: %s\n", err.c_str());
      });
      bf.note(_("The brightness keys change it too."));
      ny = bf.y() + 16;
    }
#endif
    (void)wx; (void)wy; (void)ww;
    Form nf(g, X + 24, ny, lw);
    nf.note(_("Warmer colours at night: Night Light."));
    nf.note(_("How the screen is drawn (graphics card or software): Advanced."));
    g->end();
    end();
    // one screen <-> several: what is below the view moves with it
    view->on_resize = [this, g](int dy) {
      for (int i = 0; i < g->children(); i++) {
        Fl_Widget *w = g->child(i);
        if (w != view && w->y() >= view->y() + view->h() - dy) w->position(w->x(), w->y() + dy);
      }
      g->init_sizes();
      redraw();
    };
    if (view->h() != DisplaysView::kHeight) view->on_resize(view->h() - DisplaysView::kHeight);
    outputs::on_changed = [this] { view->refresh(); };
  }
  ~DisplaysPage() { outputs::on_changed = nullptr; }
};

Fl_Group *create(int x, int y, int w, int h) { return new DisplaysPage(x, y, w, h); }

} // namespace

PageInfo page_displays() {
  return {"displays", _("Displays"), "video-display", "Hardware",
          _("display monitor screen resolution scale hidpi rotation position output refresh brightness backlight"), create};
}

} // namespace fld
