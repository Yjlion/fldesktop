// Firewall: the firewalld view of fl-firewall
#include "fld/i18n.h"
#include "fw_ui.h"
#include "fwclient.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

class FirewallPage : public Fl_Group {
public:
  FirewallPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Firewall"), cy);
    resizable(g);
    fw::FirewallView *v = new fw::FirewallView(X + 16, cy, W - 32, H - (cy - Y) - 12);
    g->resizable(v);
    g->end();
    end();
    fw::Client::get().on_changed = [v] { v->refresh(); };
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new FirewallPage(x, y, w, h); }

} // namespace

PageInfo page_firewall() {
  return {"firewall", _("Firewall"), "fl-firewall", "Network",
          _("firewall firewalld zone port service security masquerade forwarding nat"), create};
}

} // namespace fld
