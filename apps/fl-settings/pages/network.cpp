// Network: the NetworkManager view of fl-network, and Proxy as a tab
#include "fld/i18n.h"
#include <FL/Fl.H>

#include <FL/Fl_Tabs.H>

#include "fld/theme.h"
#include "fld/widgets.h"
#include "network_ui.h"
#include "nmclient.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {

Fl_Group *proxy_tab(int x, int y, int w, int h);  // pages/proxy.cpp

namespace {

class NetworkPage : public Fl_Group {
public:
  NetworkPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Network"), cy);
    resizable(g);
    nm::Client::get().start();
    // the connections, and Proxy (pages/proxy.cpp) as the second tab
    Fl_Tabs *tabs = new fld::Tabs(X + 8, cy - 8, W - 16, H - (cy - Y));
    int ty = cy - 8 + 30, th = H - (ty - Y);
    nm::NetworkView *v = new nm::NetworkView(X + 8, ty, W - 16, th);
    v->label(_("Connections"));
    Fl_Group *proxy = proxy_tab(X + 8, ty, W - 16, th);
    proxy->label(_("Proxy"));
    tabs->end();
    tabs->resizable(v);
    g->resizable(tabs);
    g->end();
    end();
    nm::Client::get().on_changed = [v] { v->refresh(); };
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new NetworkPage(x, y, w, h); }

} // namespace

PageInfo page_network() {
  return {"network", _("Network"), "network-workgroup", "Network",
          _("network wifi wi-fi wireless ethernet wired ip address dns gateway vpn networkmanager proxy"), create};
}

} // namespace fld
