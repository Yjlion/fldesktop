// fl-panel: night light ([NightLight] in desktop.ini, applied by
// fl-nightlight). A moon button shows while night light is on; a click or the
// panel's menu turns it on and off.
#include "fld/config_watch.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "panel.h"

namespace {

fld::FlatButton *g_btn = nullptr;

bool enabled() {
  fld::Ini c;
  c.load(fld::desktop_config_path());
  return c.get_bool("NightLight", "Enabled", false);
}

void set_enabled(bool on) {
  fld::Ini c;
  c.load(fld::desktop_config_path());
  c.set_bool("NightLight", "Enabled", on);
  c.save(fld::desktop_config_path());
  // fl-nightlight is started with the session; start it if it is not running
  // (it was installed meanwhile, or it was stopped)
  if (on) fld::spawn({"sh", "-c", "pgrep -u \"$(id -u)\" -x fl-nightlight >/dev/null || exec fl-nightlight"});
}

void update() {
  if (!g_btn) return;
  bool on = enabled();
  if (on == (g_btn->visible() != 0)) return;
  if (on) g_btn->show();
  else g_btn->hide();
  relayout();
}

} // namespace

bool nightlight_available() { return !fld::which("fl-nightlight").empty(); }

void nightlight_menu_items(fld::Menu &m) {
  if (!nightlight_available()) return;
  bool on = enabled();
  m.toggle("Night Light", on, [on] { set_enabled(!on); });
}

fld::FlatButton *nightlight_indicator_create() {
  g_btn = new fld::FlatButton(0, 0, 34, PANEL_H - 4);
  g_btn->color(panel_bg());
  g_btn->labelcolor(panel_fg());
  g_btn->hover_color(panel_hover());
  if (Fl_Image *i = fld::symbolic_icon("weather-clear-night", 18, panel_fg())) g_btn->icon_image(i);
  else g_btn->icon_name("weather-clear-night", 20);
  g_btn->tooltip("Night light is on. Click to turn it off.");
  g_btn->callback([](Fl_Widget *, void *) { set_enabled(false); });
  panel_context_menu(g_btn, [](fld::Menu &m) {
    nightlight_menu_items(m);
    m.item("Night Light Settings…", [] { fld::spawn({"fl-settings", "displays"}); }).divider();
    panel_menu_items(m);
  });
  if (!enabled()) g_btn->hide();
  static fld::ConfigWatcher watch(fld::desktop_config_path(), update);
  return g_btn;
}
