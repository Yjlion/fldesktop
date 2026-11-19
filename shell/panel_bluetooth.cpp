// fl-panel: the Bluetooth button (shown when there is an adapter).
// The panel also hosts the pairing agent, so devices can pair at any time.
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/fl_draw.H>

#include <string>
#include <vector>

#include "bt_ui.h"
#include "btclient.h"
#include "fld/icons.h"
#include "fld/notify.h"
#include "fld/util.h"
#include "panel.h"

namespace {

class BtButton : public fld::FlatButton {
public:
  using fld::FlatButton::FlatButton;
  bool connected = false;
protected:
  void draw() override {
    fld::FlatButton::draw();
    if (connected) {
      int cx = x() + w() - 10, cy = y() + h() - 11;
      fl_color(fld::palette().accent);
      fl_pie(cx - 4, cy - 4, 8, 8, 0, 360);
    }
  }
};

BtButton *g_btn = nullptr;
std::string g_tip;

void update() {
  const bt::State &st = bt::Client::get().state();
  const bt::Adapter *a = st.adapter();
  bool vis = st.running && a;
  if (vis) {
    bool on = a->powered;
    int n = st.connected_count();
    g_btn->connected = on && n > 0;
    Fl_Image *img = fld::symbolic_icon(on ? "bluetooth-active" : "bluetooth-disabled", 18, panel_fg());
    g_btn->icon_image(img);
    if (!img) g_btn->copy_label("BT");
    if (!on) g_tip = "Bluetooth is off";
    else if (n == 0) g_tip = "Bluetooth is on";
    else {
      g_tip = "Connected to ";
      bool first = true;
      for (auto &d : st.devices)
        if (d.connected) {
          g_tip += (first ? "" : ", ") + d.display_name() + (d.battery >= 0 ? " (" + std::to_string(d.battery) + "%)" : "");
          first = false;
        }
    }
    g_btn->tooltip(g_tip.c_str());
  }
  if (vis != (bool)g_btn->visible()) {
    vis ? g_btn->show() : g_btn->hide();
    relayout();
  }
  g_btn->redraw();
}

void bt_cb(Fl_Widget *w, void *) {
  const bt::State &st = bt::Client::get().state();
  const bt::Adapter *a = st.adapter();
  if (!a) return;
  std::vector<const bt::Device *> paired;
  for (auto &d : st.devices)
    if (d.paired) paired.push_back(&d);
  const int W = 320;
  int rows = a->powered ? std::max<int>(1, paired.size()) : 0;
  int H = 44 + rows * 44 + 46;
  PanelPopup pop(W, std::min(H, 440));
  Fl_Check_Button *power = new Fl_Check_Button(10, 8, 200, 28, "Bluetooth");
  power->labelfont(FL_HELVETICA_BOLD);
  power->value(a->powered);
  power->callback([](Fl_Widget *b, void *) {
    bt::Client::get().set_powered(((Fl_Check_Button *)b)->value());
    b->window()->hide();
  });
  static std::vector<std::string> labels, paths;
  labels.clear();
  paths.clear();
  labels.reserve(paired.size() * 2);
  paths.reserve(paired.size());
  int y = 44;
  if (a->powered) {
    for (auto *d : paired) {
      if (y > 440 - 90) break;
      Fl_Box *ic = new Fl_Box(10, y + 6, 28, 28);
      ic->image(fld::icon_first({bt::device_icon(d->icon, d->device_class), "bluetooth"}, 24));
      labels.push_back(fld::escape_at(d->display_name()));
      Fl_Box *n = new Fl_Box(44, y, W - 150, 22, labels.back().c_str());
      n->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
      labels.push_back(fld::escape_at(bt::device_status(*d)));
      Fl_Box *s = new Fl_Box(44, y + 20, W - 150, 20, labels.back().c_str());
      s->labelsize(12);
      s->labelcolor(fld::palette().fg_dim);
      s->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
      paths.push_back(d->path);
      Fl_Button *c = new Fl_Button(W - 104, y + 7, 94, 28, d->connected ? "Disconnect" : "Connect");
      c->labelsize(12);
      c->callback([](Fl_Widget *b, void *p) {
        std::string path = *(std::string *)p;
        const bt::Device *d = bt::Client::get().state().device(path);
        if (!d) return;
        auto done = [](const std::string &e) {
          if (!e.empty()) {
            fld::Notification n;
            n.summary = "Bluetooth";
            n.body = e;
            n.icon = "bluetooth-disabled";
            n.transient = true;
            fld::notify(n);
          }
        };
        if (d->connected) bt::Client::get().disconnect(path, done);
        else bt::Client::get().connect(path, done);
        b->window()->hide();
      }, &paths.back());
      y += 44;
    }
    if (paired.empty()) {
      Fl_Box *none = new Fl_Box(10, y, W - 20, 40, "No paired devices");
      none->labelcolor(fld::palette().fg_dim);
      y += 44;
    }
  }
  Fl_Button *settings = new Fl_Button(W - 160, pop.h() - 38, 150, 28, "Bluetooth Settings");
  settings->labelsize(12);
  settings->callback([](Fl_Widget *b, void *) {
    fld::spawn({"fl-bluetooth"});
    b->window()->hide();
  });
  pop.end();
  pop.popup_at(w, false);
}

} // namespace

fld::FlatButton *bluetooth_indicator_create() {
  g_btn = new BtButton(0, 0, 34, PANEL_H - 4);
  g_btn->color(panel_bg());
  g_btn->labelcolor(panel_fg());
  g_btn->hover_color(panel_hover());
  g_btn->callback(bt_cb);
  set_context_menu(g_btn, [](fld::Menu &m) {
    const bt::Adapter *a = bt::Client::get().state().adapter();
    bool on = a && a->powered;
    m.toggle("Bluetooth", on, [on] { bt::Client::get().set_powered(!on); }, a ? 0 : FL_MENU_INACTIVE).divider();
    m.item("Bluetooth Devices…", [] { fld::spawn({"fl-bluetooth"}); });
    m.item("Bluetooth Settings…", [] { fld::spawn({"fl-settings", "bluetooth"}); }).divider();
    panel_menu_items(m);
  });
  g_btn->hide();
  bt::Client &c = bt::Client::get();
  c.on_changed = update;
  c.on_connection = [](const bt::Device &d, bool connected) {
    fld::Notification n;
    n.app_name = "Bluetooth";
    n.desktop_entry = "fl-bluetooth";
    n.summary = d.display_name();
    n.body = connected ? "Connected" : "Disconnected";
    if (connected && d.battery >= 0) n.body += " · battery " + std::to_string(d.battery) + "%";
    n.icon = bt::device_icon(d.icon, d.device_class);
    n.category = connected ? "device.added" : "device.removed";
    n.transient = true;
    fld::notify(n);
  };
  c.start_agent(bt::ask_agent);
  return g_btn;
}
