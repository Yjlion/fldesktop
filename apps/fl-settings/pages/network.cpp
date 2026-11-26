// Network: the NetworkManager view of fl-network
#include "fld/i18n.h"
#include <FL/Fl.H>

#include "fld/theme.h"
#include "network_ui.h"
#include "nmclient.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

class NetworkPage : public Fl_Group {
public:
  NetworkPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Network"), cy);
    resizable(g);
    nm::Client::get().start();
    nm::NetworkView *v = new nm::NetworkView(X + 8, cy - 8, W - 16, H - (cy - Y));
    g->resizable(v);
    g->end();
    end();
    nm::Client::get().on_changed = [v] { v->refresh(); };
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new NetworkPage(x, y, w, h); }

} // namespace

PageInfo page_network() {
  return {"network", _("Network"), "network-workgroup", "Network",
          _("network wifi wi-fi wireless ethernet wired ip address dns gateway vpn networkmanager"), create};
}

} // namespace fld
