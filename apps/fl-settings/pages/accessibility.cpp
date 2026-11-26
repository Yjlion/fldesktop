// Accessibility: seeing (contrast, text size, pointer size, zoom), hearing
// the screen (Orca), typing (sticky and bounce keys)
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Spinner.H>
#include <FL/fl_draw.H>
#include <unistd.h>

#include <cmath>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

const double kTextScales[] = {1.0, 1.25, 1.5, 2.0};
const int kCursorSizes[] = {24, 32, 48, 64};

std::string orca_autostart() { return config_home() + "/autostart/fl-orca.desktop"; }

class AccessibilityPage : public Fl_Group {
public:
  ConfigFile desk{desktop_ini_path()}, wm{wm_ini_path()};
  Fl_Check_Button *contrast, *reader, *sticky, *bounce;
  Fl_Choice *text, *cursor;
  Fl_Spinner *bounce_ms;

  Fl_Check_Button *check(Form &f, const char *label, bool v) {
    int wx, wy, ww;
    f.row("", 26, wx, wy, ww);
    Fl_Check_Button *b = new fld::CheckButton(wx, wy, ww, 26, label);
    b->value(v);
    b->callback([](Fl_Widget *, void *d) { ((AccessibilityPage *)d)->save(); }, this);
    return b;
  }

  AccessibilityPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Accessibility"), cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    auto cb = [](Fl_Widget *, void *d) { ((AccessibilityPage *)d)->save(); };

    f.heading(_("Seeing"));
    contrast = check(f, _("High contrast"), lower(desk.ini.get("Appearance", "Contrast", "normal")) == "high");
    f.row(_("Text size:"), 28, wx, wy, ww);
    text = new Fl_Choice(wx, wy, 220, 28);
    text->add(_("Default|Large (125%)|Larger (150%)|Largest (200%)"));
    double ts = desk.ini.get_double("Appearance", "TextScale", 1.0);
    int ti = 0;
    for (int i = 0; i < 4; i++)
      if (std::fabs(kTextScales[i] - ts) < 0.01) ti = i;
    text->value(ti);
    text->callback(cb, this);
    f.row(_("Pointer size:"), 28, wx, wy, ww);
    cursor = new Fl_Choice(wx, wy, 220, 28);
    cursor->add(_("Default|Large|Larger|Largest"));
    int cs = wm.ini.get_int("cursor", "size", 24), ci = 0;
    for (int i = 0; i < 4; i++)
      if (kCursorSizes[i] <= cs) ci = i;
    cursor->value(ci);
    cursor->callback(cb, this);
    f.note(_("Programs that are open take the contrast and the text size when they start again."), 2);
    f.row(_("Zoom:"), 26, wx, wy, ww);
    Fl_Box *zoom = new Fl_Box(wx, wy, ww, 26, _("Super+= zooms in, Super+- out, Super+0 back"));
    zoom->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);

    f.heading(_("Screen Reader"));
    bool have_orca = !which("orca").empty();
    reader = check(f, _("Read the screen aloud (Orca)"), have_orca && file_exists(orca_autostart()));
    if (!have_orca) {
      reader->deactivate();
      f.note(_("Install Orca for a screen reader. It reads the fldesktop programs (started after it) as well "
             "as GTK and Qt programs."), 2);
    } else {
      f.note(_("Orca reads the fldesktop programs started after it, and GTK and Qt programs."));
    }

    f.heading(_("Typing"));
    sticky = check(f, _("Sticky keys: press Shift, Ctrl, Alt or Super, then the key"),
                   wm.ini.get_bool("accessibility", "sticky_keys", false));
    int bms = wm.ini.get_int("accessibility", "bounce_keys", 0);
    f.row("", 28, wx, wy, ww);
    const char *blabel = "Bounce keys: ignore repeats within";
    fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
    int bw = (int)fl_width(blabel) + 34; // the label grows with the text size
    bounce = new fld::CheckButton(wx, wy, bw, 28, blabel);
    bounce->value(bms > 0);
    bounce->callback(cb, this);
    bounce_ms = new Fl_Spinner(wx + bw + 6, wy, 80, 28);
    bounce_ms->range(50, 2000);
    bounce_ms->step(50);
    bounce_ms->value(bms > 0 ? bms : 300);
    bounce_ms->callback(cb, this);
    Fl_Box *ms = new Fl_Box(wx + bw + 92, wy, 40, 28, "ms");
    ms->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    g->end();
    end();
    update();
  }

  void update() {
    if (bounce->value()) bounce_ms->activate();
    else bounce_ms->deactivate();
  }

  void save() {
    update();
    desk.ini.load(desk.path);
    desk.ini.set("Appearance", "Contrast", contrast->value() ? "high" : "normal");
    char buf[16];
    snprintf(buf, sizeof buf, "%.2f", kTextScales[std::max(0, text->value())]);
    desk.ini.set("Appearance", "TextScale", buf);
    desk.save();

    wm.ini.load(wm.path);
    wm.ini.set_int("cursor", "size", kCursorSizes[std::max(0, cursor->value())]);
    wm.ini.set_bool("accessibility", "sticky_keys", sticky->value());
    wm.ini.set_int("accessibility", "bounce_keys", bounce->value() ? (int)bounce_ms->value() : 0);
    save_wm(wm);

    // the screen reader: started now and at every login, or stopped
    bool want = reader->active() && reader->value();
    bool had = file_exists(orca_autostart());
    if (want && !had) {
      mkdir_p(config_home() + "/autostart");
      write_file(orca_autostart(), "[Desktop Entry]\nType=Application\nName=Screen Reader\nExec=orca\n"
                                   "OnlyShowIn=fldesktop;\nNoDisplay=true\n");
      spawn({"orca", "--replace"});
    } else if (!want && had) {
      unlink(orca_autostart().c_str());
      spawn({"pkill", "-u", std::to_string(getuid()), "-x", "orca"});
    }
    // programs (the fldesktop ones too) make their widgets readable when this is on
    if (want != had && !which("gdbus").empty())
      spawn({"gdbus", "call", "--session", "--dest", "org.a11y.Bus", "--object-path", "/org/a11y/bus", "--method",
             "org.freedesktop.DBus.Properties.Set", "org.a11y.Status", "ScreenReaderEnabled",
             want ? "<true>" : "<false>"});
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new AccessibilityPage(x, y, w, h); }

} // namespace

PageInfo page_accessibility() {
  return {"accessibility", _("Accessibility"), "preferences-desktop-accessibility", "Personal",
          _("accessibility a11y contrast text size large font zoom magnifier screen reader orca sticky bounce keys "
          "pointer cursor size"),
          create};
}

} // namespace fld
