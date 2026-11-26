// Advanced: fl-wm switches that take effect at the next login (wm.ini
// [behaviour] renderer= and xwayland=).
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <algorithm>
#include <cstdlib>
#include <string>

#include "fld/ini.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

const char *const B = "behaviour";
const char *const kRenderers[] = {"auto", "gles2", "vulkan", "pixman"};

class AdvancedPage : public Fl_Group {
public:
  ConfigFile wm{wm_ini_path()};
  Fl_Choice *renderer;
  Fl_Check_Button *xwayland;
  std::string now_note;

  AdvancedPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Advanced"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    f.note(_("These settings take effect at the next login."), 1);
    auto cb = [](Fl_Widget *, void *d) { ((AdvancedPage *)d)->save(); };

    f.heading(_("Graphics"));
    f.row(_("Drawing:"), 28, wx, wy, ww);
    renderer = new Fl_Choice(wx, wy, std::min(ww, 320), 28);
    renderer->add(_("Automatic|Graphics card (OpenGL ES)|Graphics card (Vulkan, experimental)|Software (least memory)"));
    renderer->callback(cb, this);
    const char *cur = getenv("FLWM_RENDERER");
    if (cur && *cur) now_note = format(_("Now: %s."), {cur}) + " ";
    now_note += _("Software drawing needs about 50 MB less memory on older graphics cards, but more processor "
                  "time while windows move.");
    f.note(now_note.c_str(), 3);

    f.heading(_("X11 Programs"));
    int bx, by, bw;
    f.row("", 26, bx, by, bw);
    xwayland = new fld::CheckButton(bx, by, bw, 26, _("Run X11 programs (Xwayland)"));
    xwayland->callback(cb, this);
    f.note(_("Xwayland starts with the first X11 program. Off: only Wayland programs run."), 2);

    f.space(8);
    reset_button(f.widget_x(), f.y(), wm, {{B, "renderer"}, {B, "xwayland"}}, [this] { load(); });
    g->end();
    end();
    load();
  }

  void load() {
    wm.ini.load(wm.path);
    std::string r = wm.ini.get(B, "renderer", "auto");
    renderer->value(0);
    for (int i = 0; i < 4; i++)
      if (r == kRenderers[i]) renderer->value(i);
    xwayland->value(wm.ini.get_bool(B, "xwayland", true));
  }

  void save() {
    wm.ini.load(wm.path);
    if (renderer->value() >= 0) wm.ini.set(B, "renderer", kRenderers[renderer->value()]);
    wm.ini.set_bool(B, "xwayland", xwayland->value());
    save_wm(wm);
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new AdvancedPage(x, y, w, h); }

} // namespace

PageInfo page_advanced() {
  return {"advanced", _("Advanced"), "preferences-other", "System",
          _("advanced renderer drawing graphics card gpu opengl gles vulkan software pixman xwayland x11 xorg"),
          create};
}

} // namespace fld
