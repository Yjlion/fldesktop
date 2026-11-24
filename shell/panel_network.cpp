// fl-panel: the network button's popup (wired devices, Wi-Fi networks, VPNs)
// and its right-click menu.
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/fl_draw.H>

#include <deque>
#include <string>
#include <vector>

#include "fld/icons.h"
#include "fld/text.h"
#include "fld/menus.h"
#include "fld/notify.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "network_ui.h"
#include "nmclient.h"
#include "panel.h"

namespace {

void report(const std::string &err) {
  if (err.empty()) return;
  fld::Notification n;
  n.app_name = "Network";
  n.desktop_entry = "fl-network";
  n.summary = "Network";
  n.body = err;
  n.icon = "network-error";
  n.transient = true;
  fld::notify(n);
}

const nm::Connection *saved_wifi(const std::string &ssid) {
  for (auto &c : nm::Client::get().state().connections)
    if (c.type == "802-11-wireless" && c.ssid == ssid) return &c;
  return nullptr;
}

// joins a network: a saved connection, an open one, or asks for the password
void join_wifi(const std::string &dev, const nm::AccessPoint &ap) {
  nm::Client &c = nm::Client::get();
  if (const nm::Connection *s = saved_wifi(ap.ssid)) {
    c.activate(s->uuid, dev, report);
    return;
  }
  if (ap.enterprise) {
    // 802.1X needs more than a password: the network settings handle it
    fld::spawn({"fl-network"});
    return;
  }
  std::string pw;
  if (ap.secure) {
    pw = nm::ask_wifi_password(ap.ssid);
    if (pw.empty()) return;
  }
  c.connect_wifi(dev, ap.ssid, pw, false, report);
}

// one row: an icon, a name with a line under it, and a button
struct Row {
  std::string name, sub, dev, uuid;
  nm::AccessPoint ap;
  enum Kind { WIRED, WIFI, VPN } kind;
  bool active = false;
};

std::deque<Row> g_rows; // stable addresses for the callbacks

void row_cb(Fl_Widget *b, void *d) {
  Row *r = (Row *)d;
  nm::Client &c = nm::Client::get();
  b->window()->hide();
  switch (r->kind) {
    case Row::WIRED:
      if (r->active) c.disconnect_device(r->dev, report);
      else c.activate("", r->dev, report);
      break;
    case Row::WIFI:
      if (r->active) c.disconnect_device(r->dev, report);
      else join_wifi(r->dev, r->ap);
      break;
    case Row::VPN:
      if (r->active) c.deactivate(r->uuid, report);
      else c.activate(r->uuid, "", report);
      break;
  }
}

std::string wired_status(const nm::Device &d) {
  if (d.connecting) return "Connecting…";
  if (d.connected) return d.ip4.empty() ? "Connected" : "Connected · " + d.ip4[0].substr(0, d.ip4[0].find('/'));
  if (!d.available) return "Cable unplugged";
  return "Disconnected";
}

} // namespace

void network_popup(Fl_Widget *w) {
  if (panel_popup_toggle(w)) return; // open: a second click closes it
  nm::Client &c = nm::Client::get();
  const nm::State &st = c.state();
  if (!st.running) {
    fld::spawn({"fl-network"});
    return;
  }
  g_rows.clear();
  for (auto &d : st.devices) {
    if (d.type == "ethernet" && d.managed) {
      Row r;
      r.kind = Row::WIRED;
      r.dev = d.path;
      r.name = "Wired" + (d.iface.empty() ? "" : " (" + d.iface + ")");
      r.sub = wired_status(d);
      r.active = d.connected || d.connecting;
      g_rows.push_back(r);
    }
  }
  bool have_wifi = false;
  for (auto &d : st.devices) {
    if (d.type != "wifi" || !d.managed) continue;
    have_wifi = true;
    if (!st.wireless) continue;
    c.scan(d.path); // fresh results for the next time
    int n = 0;
    for (auto &ap : d.aps) {
      if (ap.ssid.empty() || n++ >= 10) continue;
      Row r;
      r.kind = Row::WIFI;
      r.dev = d.path;
      r.ap = ap;
      r.name = ap.ssid;
      r.active = ap.active && d.connected;
      r.sub = r.active ? "Connected" : (ap.active && d.connecting) ? "Connecting…"
              : saved_wifi(ap.ssid) ? "Saved" : ap.secure ? (ap.security.empty() ? "Secured" : ap.security) : "Open";
      g_rows.push_back(r);
    }
  }
  for (auto &cn : st.connections) {
    if (cn.type != "vpn" && cn.type != "wireguard") continue;
    Row r;
    r.kind = Row::VPN;
    r.uuid = cn.uuid;
    r.name = cn.id;
    r.sub = cn.type == "wireguard" ? "WireGuard" : "VPN";
    r.active = cn.active;
    if (r.active) r.sub += " · connected";
    g_rows.push_back(r);
  }

  const int W = 340, RH = 44;
  int H = 44 + std::max<int>(1, (int)g_rows.size()) * RH + 8 + 38 + 6;
  H = std::min(H, 560);
  PanelPopup pop(W, H);
  const fld::Palette &p = fld::palette();
  Fl_Check_Button *net = new fld::CheckButton(10, 8, 150, 28, "Networking");
  net->labelfont(FL_HELVETICA_BOLD);
  net->value(st.networking);
  net->callback([](Fl_Widget *b, void *) {
    nm::Client::get().set_networking(((Fl_Check_Button *)b)->value(), report);
    b->window()->hide();
  });
  if (have_wifi) {
    Fl_Check_Button *wifi = new fld::CheckButton(W - 100, 8, 90, 28, "Wi-Fi");
    wifi->value(st.wireless);
    if (!st.wireless_hw || !st.networking) wifi->deactivate();
    wifi->callback([](Fl_Widget *b, void *) {
      nm::Client::get().set_wireless(((Fl_Check_Button *)b)->value(), report);
      b->window()->hide();
    });
  }
  int y = 44;
  if (g_rows.empty()) {
    Fl_Box *none = new Fl_Box(10, y, W - 20, RH, st.networking ? "No networks found" : "Networking is off");
    none->labelcolor(p.fg_dim);
    y += RH;
  }
  Row::Kind last = Row::WIRED;
  bool first = true;
  for (auto &r : g_rows) {
    if (y + RH > H - 52) break;
    if (!first && r.kind != last) {
      Fl_Box *sep = new Fl_Box(10, y + 1, W - 20, 1);
      sep->box(FL_FLAT_BOX);
      sep->color(p.border);
    }
    first = false;
    last = r.kind;
    const char *icon = r.kind == Row::WIRED ? "network-wired"
                       : r.kind == Row::VPN ? "network-vpn"
                                            : nm::signal_icon(r.ap.strength);
    Fl_Box *ic = new Fl_Box(10, y + 8, 28, 28);
    ic->image(fld::icon_first({icon, "network-wireless", "fl-network"}, 22));
    Fl_Box *n = new Fl_Box(44, y + 2, W - 160, 22);
    n->labelfont(r.active ? FL_HELVETICA_BOLD : FL_HELVETICA);
    // a long name ends with "…"; the whole of it in the tooltip
    fl_font(n->labelfont(), n->labelsize());
    std::string shown = fld::ellipsize(r.name, n->w() - 4);
    n->copy_label(fld::escape_at(shown).c_str());
    if (shown != r.name) n->copy_tooltip(r.name.c_str());
    n->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    std::string sub = r.sub;
    Fl_Box *s = new Fl_Box(44, y + 22, W - 160, 18);
    s->copy_label(fld::escape_at(sub).c_str());
    s->labelsize(12);
    s->labelcolor(p.fg_dim);
    s->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    // a lock before the security of a protected network
    if (r.kind == Row::WIFI && r.ap.secure)
      if (Fl_Image *lock = fld::symbolic_icon("network-wireless-encrypted", 12, p.fg_dim)) {
        s->image(lock);
        s->align(s->align() | FL_ALIGN_IMAGE_NEXT_TO_TEXT);
      }
    Fl_Button *b = new Fl_Button(W - 108, y + 8, 98, 28, r.active ? "Disconnect" : "Connect");
    b->labelsize(12);
    b->callback(row_cb, &r);
    y += RH;
  }
  Fl_Button *settings = new Fl_Button(W - 160, H - 38, 150, 28, "Network Settings");
  settings->labelsize(12);
  settings->callback([](Fl_Widget *b, void *) {
    fld::spawn({"fl-network"});
    b->window()->hide();
  });
  pop.end();
  pop.popup_at(w, false);
}

void network_context_menu(fld::Menu &m) {
  nm::Client &c = nm::Client::get();
  const nm::State &st = c.state();
  bool have_wifi = false;
  for (auto &d : st.devices)
    if (d.type == "wifi" && d.managed) have_wifi = true;
  for (auto &d : st.devices)
    if (d.connected && (d.type == "wifi" || d.type == "ethernet")) {
      std::string path = d.path;
      m.item("Disconnect " + fld::Menu::text(d.active_id.empty() ? d.iface : d.active_id),
             [path] { nm::Client::get().disconnect_device(path, report); });
    }
  m.toggle("Networking", st.networking, [on = !st.networking] { nm::Client::get().set_networking(on, report); });
  if (have_wifi)
    m.toggle("Wi-Fi", st.wireless, [on = !st.wireless] { nm::Client::get().set_wireless(on, report); },
             st.wireless_hw && st.networking ? 0 : FL_MENU_INACTIVE);
  m.divider();
  m.item("Network Settings…", [] { fld::spawn({"fl-network"}); });
}
