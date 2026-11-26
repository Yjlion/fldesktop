// Printers: the CUPS view of fl-printers
#include "fld/i18n.h"
#include "printers_ui.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

class PrintersPage : public Fl_Group {
public:
  PrintersPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("Printers"), cy);
    resizable(g);
    prn::PrintersView *v = new prn::PrintersView(X + 16, cy, W - 32, H - (cy - Y) - 12);
    g->resizable(v);
    g->end();
    end();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new PrintersPage(x, y, w, h); }

} // namespace

PageInfo page_printers() {
  return {"printers", _("Printers"), "printer", "Hardware",
          _("printers printing cups queue jobs paper scanner driver ipp default printer"), create};
}

} // namespace fld
