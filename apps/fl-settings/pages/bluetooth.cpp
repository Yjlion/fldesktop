// Bluetooth: the BlueZ view of fl-bluetooth
#include "fld/i18n.h"
#include <algorithm>

#include "bt_ui.h"
#include "btclient.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

class BluetoothPage : public Fl_Group {
public:
  BluetoothPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Bluetooth"), cy);
    resizable(g);
    bt::BluetoothView *v = new bt::BluetoothView(X + 24, cy, std::min(W - 48, 720), H - (cy - Y) - 12);
    g->resizable(v);
    g->end();
    end();
    bt::Client::get().on_changed = [v] { v->refresh(); };
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new BluetoothPage(x, y, w, h); }

} // namespace

PageInfo page_bluetooth() {
  return {"bluetooth", _("Bluetooth"), "bluetooth-active", "Hardware",
          _("bluetooth pair headset headphones mouse keyboard speaker wireless"), create};
}

} // namespace fld
