// fl-panel: quick settings. One button opens a popup with the switches used
// most: Wi-Fi, Bluetooth, Do Not Disturb, Night Light, the power mode and the
// volume. Each row shows only when its service is there.
#include "fld/i18n.h"
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Hor_Slider.H>
#include <FL/Fl_Round_Button.H>
#include <string>
#include <vector>

#include "fld/util.h"
#include "fld/widgets.h"
#include "panel.h"

#ifdef FLD_HAVE_NM
#include "nmclient.h"
#endif
#ifdef FLD_HAVE_BLUETOOTH
#include "btclient.h"
#endif
#ifdef FLD_HAVE_POWER
#include "upclient.h"
#endif

namespace {

fld::FlatButton *g_btn = nullptr;
const int W = 300, ROW = 34, PAD = 12;

Fl_Check_Button *toggle(int &y, const char *label, bool on, Fl_Callback *cb) {
  Fl_Check_Button *b = new fld::CheckButton(PAD, y, W - 2 * PAD, 28, label);
  b->value(on);
  b->callback(cb);
  y += ROW;
  return b;
}

void heading(int &y, const char *text) {
  Fl_Box *h = new Fl_Box(PAD, y, W - 2 * PAD, 22, text);
  h->labelfont(FL_HELVETICA_BOLD);
  h->labelsize(12);
  h->labelcolor(fld::palette().fg_dim);
  h->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  y += 24;
}

void open_popup(Fl_Widget *btn, void *) {
  if (panel_popup_toggle(btn)) return;
  // the rows first, to know the height
  int y = PAD;
  PanelPopup *p = new PanelPopup(W, 400);
  p->begin();
  heading(y, _("Quick Settings"));
#ifdef FLD_HAVE_NM
  {
    const nm::State &st = nm::Client::get().state();
    if (st.running && st.wireless_hw)
      toggle(y, _("Wi-Fi"), st.wireless, [](Fl_Widget *w, void *) {
        nm::Client::get().set_wireless(((Fl_Check_Button *)w)->value() != 0);
      });
  }
#endif
#ifdef FLD_HAVE_BLUETOOTH
  {
    const bt::Adapter *a = bt::Client::get().state().adapter();
    if (a)
      toggle(y, _("Bluetooth"), a->powered, [](Fl_Widget *w, void *) {
        bt::Client::get().set_powered(((Fl_Check_Button *)w)->value() != 0);
      });
  }
#endif
#ifdef FLD_HAVE_NOTIFY
  toggle(y, _("Do Not Disturb"), dnd_on(), [](Fl_Widget *w, void *) { dnd_set(((Fl_Check_Button *)w)->value() != 0); });
#endif
  if (nightlight_available())
    toggle(y, _("Night Light"), nightlight_on(),
           [](Fl_Widget *w, void *) { nightlight_set(((Fl_Check_Button *)w)->value() != 0); });
#ifdef FLD_HAVE_POWER
  {
    const up::State &st = up::Client::get().state();
    if (st.profiles_available && !st.profiles.empty()) {
      y += 4;
      heading(y, _("Power Mode"));
      static std::vector<std::string> names;
      names = st.profiles;
      for (size_t i = 0; i < names.size(); i++) {
        Fl_Round_Button *r = new Fl_Round_Button(PAD, y, W - 2 * PAD, 26);
        r->copy_label(up::profile_label(names[i]).c_str());
        r->type(FL_RADIO_BUTTON);
        r->value(names[i] == st.profile);
        r->callback([](Fl_Widget *, void *d) { up::Client::get().set_profile(*(std::string *)d); }, &names[i]);
        y += 28;
      }
    }
  }
#endif
  int vol = panel_volume_pct();
  if (vol >= 0) {
    y += 4;
    heading(y, _("Volume"));
    Fl_Hor_Slider *s = new Fl_Hor_Slider(PAD, y, W - 2 * PAD, 24);
    s->bounds(0, 100);
    s->step(1);
    s->value(panel_volume_muted() ? 0 : std::min(vol, 100));
    s->callback([](Fl_Widget *w, void *) { panel_volume_set((int)((Fl_Slider *)w)->value()); });
    y += 32;
  }
  y += 4;
  Fl_Button *more = new Fl_Button(PAD, y, W - 2 * PAD, 30, _("All Settings…"));
  more->callback([](Fl_Widget *w, void *) {
    w->window()->hide();
    fld::spawn({"fl-settings"});
  });
  y += 30 + PAD;
  p->end();
  p->size(W, y);
  p->popup_at(btn, false);
  Fl::delete_widget(p);
}

} // namespace

fld::FlatButton *quick_settings_create() {
  g_btn = make_btn(34, "preferences-system", _("Quick settings"), open_popup);
  panel_icon(g_btn, "emblem-system-symbolic", "preferences-system");
  return g_btn;
}
