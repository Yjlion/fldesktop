// fl-panel: the network button's popup (wired devices, Wi-Fi networks, VPNs)
// and its right-click menu.
//
// The popup has the Wi-Fi switch at the top, then what is connected (with
// its address and a Disconnect button), then the networks one can join: a
// click on a row joins it. It stays open while connecting and follows
// NetworkManager's changes (network_popup_refresh), so "Connecting…" and the
// results of the scan it starts show up in place.
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "fld/icons.h"
#include "fld/text.h"
#include "fld/theme.h"
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

void close_popup() {
  if (PanelPopup *p = PanelPopup::open_popup()) p->hide();
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
    close_popup();
    fld::spawn({"fl-network"});
    return;
  }
  std::string pw;
  if (ap.secure) {
    // the password dialog takes the focus, which would close the popup anyway
    close_popup();
    pw = nm::ask_wifi_password(ap.ssid);
    if (pw.empty()) return;
  }
  c.connect_wifi(dev, ap.ssid, pw, false, report);
}

struct Row {
  std::string name, sub, tip, dev, uuid;
  nm::AccessPoint ap;
  enum Kind { WIRED, WIFI, VPN } kind;
  bool active = false;     // connected or connecting: in the top section
  bool connecting = false;
  bool saved = false;
};

std::deque<Row> g_rows; // stable addresses for the callbacks
Fl_Widget *g_button = nullptr; // the panel's network button (the popup's owner)

// what a click does: connects a row of the list, disconnects one at the top
void row_action(Row *r) {
  nm::Client &c = nm::Client::get();
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

std::string ipv4_of(const nm::Device &d) {
  for (auto &a : d.ip4)
    if (a.compare(0, 4, "127.") != 0) return a.substr(0, a.find('/'));
  return "";
}

std::string wired_status(const nm::Device &d) {
  if (d.connecting) return "Connecting…";
  if (d.connected) {
    std::string ip = ipv4_of(d);
    return ip.empty() ? "Connected" : "Connected · " + ip;
  }
  if (!d.available) return "Cable unplugged";
  return "";
}

const char *row_icon(const Row &r) {
  return r.kind == Row::WIRED ? "network-wired" : r.kind == Row::VPN ? "network-vpn" : nm::signal_icon(r.ap.strength);
}

// the row's icon in the text colour (the theme's full-colour network icons
// are dark: they vanish on the dark theme)
Fl_Image *row_image(const Row &r) {
  if (Fl_Image *i = fld::symbolic_icon(row_icon(r), 22, fld::palette().fg)) return i;
  return fld::icon_first({row_icon(r), "network-wireless", "fl-network"}, 22);
}

Fl_Image *lock_icon(Fl_Color c) {
  return fld::symbolic_icon("system-lock-screen", 12, c); // a padlock, in the fldesktop theme
}

// One network of the list: icon, name, a dim line under it and a lock for
// protected Wi-Fi. The whole row is the button (named after the network for
// screen readers); it is highlighted under the pointer and when focused.
class NetRow : public Fl_Button {
public:
  NetRow(int x, int y, int w, int h, Row *r) : Fl_Button(x, y, w, h), row_(r) {
    box(FL_FLAT_BOX);
    down_box(FL_FLAT_BOX);
    color(fld::palette().bg2);
    copy_label(fld::escape_at(r->name).c_str());
    labeltype(FL_NO_LABEL); // drawn by draw(); the label names it for AT-SPI
    std::string tip = r->name;
    if (!r->tip.empty()) tip += "\n" + r->tip;
    copy_tooltip(tip.c_str());
    callback([](Fl_Widget *, void *d) { row_action((Row *)d); }, r);
  }
  int handle(int e) override {
    switch (e) {
      case FL_ENTER: hover_ = true; redraw(); return 1;
      case FL_LEAVE: hover_ = false; redraw(); break;
      case FL_FOCUS:
      case FL_UNFOCUS: redraw(); break;
      default: break;
    }
    return Fl_Button::handle(e);
  }

protected:
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(color());
    fl_rectf(x(), y(), w(), h());
    bool focused = Fl::focus() == this;
    if (value() || hover_ || focused) {
      fl_color(value() ? fl_color_average(color(), p.accent, 0.65f)
                       : fl_color_average(color(), p.dark ? FL_WHITE : FL_BLACK, 0.9f));
      fl_rounded_rectf(x() + 2, y() + 1, w() - 4, h() - 2, 5);
    }
    if (focused) {
      fl_color(p.accent);
      fl_rounded_rect(x() + 2, y() + 1, w() - 4, h() - 2, 5);
    }
    if (Fl_Image *ic = row_image(*row_))
      ic->draw(x() + 10, y() + (h() - ic->h()) / 2);
    int tx = x() + 44, right = x() + w() - 10;
    Fl_Image *lock = row_->kind == Row::WIFI && row_->ap.secure ? lock_icon(p.fg_dim) : nullptr;
    if (lock) {
      lock->draw(right - lock->w(), y() + (h() - lock->h()) / 2);
      right -= lock->w() + 8;
    }
    bool two = !row_->sub.empty();
    fl_font(FL_HELVETICA, fld::ui_font_size());
    fl_color(p.fg);
    std::string n = fld::escape_at(fld::ellipsize(row_->name, right - tx));
    fl_draw(n.c_str(), tx, y() + (two ? 3 : 0), right - tx, two ? h() / 2 : h(),
            FL_ALIGN_LEFT | (two ? FL_ALIGN_BOTTOM : 0) | FL_ALIGN_CLIP);
    if (two) {
      fl_font(FL_HELVETICA, std::max(10, fld::ui_font_size() - 2));
      fl_color(p.fg_dim);
      fl_draw(fld::escape_at(row_->sub).c_str(), tx, y() + h() / 2 + 2, right - tx, h() / 2 - 6,
              FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_CLIP);
    }
  }

private:
  Row *row_;
  bool hover_ = false;
};

// the rows from NetworkManager's state: connected and connecting first, then
// wired and VPN connections, then Wi-Fi networks (saved ones first, then by
// signal)
void collect_rows(const nm::State &st, bool &have_wifi) {
  g_rows.clear();
  have_wifi = false;
  for (auto &d : st.devices) {
    if (d.type != "ethernet" || !d.managed) continue;
    Row r;
    r.kind = Row::WIRED;
    r.dev = d.path;
    r.name = "Wired" + (d.iface.empty() ? "" : " (" + d.iface + ")");
    r.sub = wired_status(d);
    r.active = d.connected || d.connecting;
    r.connecting = d.connecting;
    // an unplugged cable can't be connected: only shown while it is active
    if (!d.available && !r.active) continue;
    g_rows.push_back(r);
  }
  for (auto &cn : st.connections) {
    if (cn.type != "vpn" && cn.type != "wireguard") continue;
    Row r;
    r.kind = Row::VPN;
    r.uuid = cn.uuid;
    r.name = cn.id;
    r.active = cn.active;
    r.sub = r.active ? "Connected · VPN" : cn.type == "wireguard" ? "WireGuard" : "VPN";
    g_rows.push_back(r);
  }
  std::vector<Row> wifi;
  for (auto &d : st.devices) {
    if (d.type != "wifi" || !d.managed) continue;
    have_wifi = true;
    if (!st.wireless) continue;
    for (auto &ap : d.aps) {
      if (ap.ssid.empty()) continue;
      Row r;
      r.kind = Row::WIFI;
      r.dev = d.path;
      r.ap = ap;
      r.name = ap.ssid;
      r.connecting = ap.active && d.connecting;
      r.active = ap.active && (d.connected || d.connecting);
      r.saved = saved_wifi(ap.ssid) != nullptr;
      if (r.connecting) r.sub = "Connecting…";
      else if (r.active) {
        std::string ip = ipv4_of(d);
        r.sub = ip.empty() ? "Connected" : "Connected · " + ip;
      } else if (r.saved) r.sub = "Saved";
      else if (!ap.secure) r.sub = "Open";
      r.tip = ap.secure ? (ap.security.empty() ? "Secured" : ap.security) : "Open network";
      if (ap.frequency) r.tip += ap.frequency >= 5000 ? " · 5 GHz" : " · 2.4 GHz";
      r.tip += " · signal " + std::to_string(ap.strength) + "%";
      wifi.push_back(r);
    }
  }
  std::stable_sort(wifi.begin(), wifi.end(), [](const Row &a, const Row &b) {
    if (a.active != b.active) return a.active;
    if (a.saved != b.saved) return a.saved;
    return a.ap.strength > b.ap.strength;
  });
  if (wifi.size() > 30) wifi.resize(30);
  for (auto &r : wifi) g_rows.push_back(r);
  // connected ones on top, in the order above
  std::stable_partition(g_rows.begin(), g_rows.end(), [](const Row &r) { return r.active; });
}

const int W = 340, RH = 44, HEAD = 44, FOOT = 46, MAX_H = 520;

// the popup's height when it opens: room for what is there now, and for a few
// networks the scan it starts may find; later changes scroll inside it
int popup_height(const nm::State &st, bool have_wifi) {
  int active = 0, others = 0;
  for (auto &r : g_rows) (r.active ? active : others)++;
  int h = HEAD + active * RH + (active ? 8 : 0) + FOOT;
  if (!st.networking) return h + 80;
  if (others || have_wifi) h += 26 + std::max(others, have_wifi && st.wireless ? 5 : 1) * RH;
  else if (!active) h += RH;
  return std::min(h, MAX_H);
}

Fl_Scroll *g_list = nullptr;

// fills the popup with the current state (on opening and on every change)
void build(PanelPopup *pop) {
  const nm::State &st = nm::Client::get().state();
  bool have_wifi = false;
  collect_rows(st, have_wifi);
  const fld::Palette &p = fld::palette();
  const int H = pop->h();
  pop->begin();

  // header: the Wi-Fi switch (turning all networking off is in the
  // right-click menu and in Network Settings: not one stray click away)
  if (have_wifi && st.networking) {
    Fl_Check_Button *wifi = new fld::CheckButton(10, 8, 150, 28, _("Wi-Fi"));
    wifi->labelfont(FL_HELVETICA_BOLD);
    wifi->value(st.wireless);
    wifi->tooltip(st.wireless ? "Turn Wi-Fi off" : "Turn Wi-Fi on");
    if (!st.wireless_hw) {
      wifi->deactivate();
      wifi->tooltip(_("Wi-Fi is turned off by a hardware switch"));
    }
    wifi->callback([](Fl_Widget *b, void *) {
      nm::Client::get().set_wireless(((Fl_Check_Button *)b)->value(), report);
    });
  } else {
    Fl_Box *title = new Fl_Box(14, 8, 200, 28, have_wifi ? "Wi-Fi" : "Network");
    title->labelfont(FL_HELVETICA_BOLD);
    title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  }
  int y = HEAD;

  if (!st.networking) {
    Fl_Box *off = new Fl_Box(14, y, W - 28, 36, _("Networking is off"));
    off->labelcolor(p.fg_dim);
    off->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    Fl_Button *on = new Fl_Button(14, y + 40, 110, 28, _("Turn On"));
    on->labelsize(12);
    on->callback([](Fl_Widget *, void *) { nm::Client::get().set_networking(true, report); });
  } else {
    // connected (and connecting): each with a Disconnect button
    bool any_active = false;
    for (auto &r : g_rows) {
      if (!r.active) continue;
      any_active = true;
      Fl_Box *ic = new Fl_Box(10, y + 8, 28, 28);
      ic->image(row_image(r));
      Fl_Box *n = new Fl_Box(44, y + 3, W - 160, 22);
      n->labelfont(FL_HELVETICA_BOLD);
      fl_font(n->labelfont(), n->labelsize());
      n->copy_label(fld::escape_at(fld::ellipsize(r.name, n->w() - 4)).c_str());
      n->copy_tooltip(r.name.c_str());
      n->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
      Fl_Box *s = new Fl_Box(44, y + 23, W - 160, 18);
      s->copy_label(fld::escape_at(r.sub).c_str());
      s->labelsize(12);
      s->labelcolor(p.fg_dim);
      s->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
      Fl_Button *b = new Fl_Button(W - 108, y + 8, 98, 28, r.connecting ? "Cancel" : "Disconnect");
      b->labelsize(12);
      b->copy_tooltip(((r.connecting ? "Stop connecting to " : "Disconnect from ") + r.name).c_str());
      b->callback([](Fl_Widget *, void *d) { row_action((Row *)d); }, &r);
      y += RH;
    }
    if (any_active) {
      Fl_Box *sep = new Fl_Box(10, y + 3, W - 20, 1);
      sep->box(FL_FLAT_BOX);
      sep->color(p.border);
      y += 8;
    }
    int others = 0;
    for (auto &r : g_rows) others += !r.active;
    if (others || (have_wifi && st.wireless)) {
      Fl_Box *h = new Fl_Box(14, y, W - 28, 22, _("Available networks"));
      h->labelsize(12);
      h->labelcolor(p.fg_dim);
      h->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
      y += 26;
    }
    int list_h = std::max(RH, H - FOOT - y);
    g_list = new Fl_Scroll(1, y, W - 2, list_h); // inside the popup's border
    g_list->type(Fl_Scroll::VERTICAL);
    g_list->box(FL_FLAT_BOX);
    g_list->color(p.bg2);
    g_list->scrollbar_size(10);
    int ly = y;
    Row::Kind last = Row::WIRED;
    bool first = true;
    for (auto &r : g_rows) {
      if (r.active) continue;
      if (!first && r.kind != last) ly += 4; // a little space between kinds
      first = false;
      last = r.kind;
      new NetRow(4, ly, W - 18, RH, &r);
      ly += RH;
    }
    if (first) {
      const char *msg = have_wifi && !st.wireless ? "Wi-Fi is off"
                        : have_wifi                 ? "Looking for networks…"
                                                    : any_active ? "" : "No networks found";
      Fl_Box *none = new Fl_Box(14, y, W - 28, RH, msg);
      none->labelcolor(p.fg_dim);
      none->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    }
    g_list->end();
  }

  // footer
  if (have_wifi && st.networking && st.wireless) {
    fld::FlatButton *hidden = new fld::FlatButton(8, H - 38, 130, 28, _("Hidden Network…"));
    hidden->labelsize(12);
    hidden->color(p.bg2);
    hidden->tooltip(_("Join a network that does not show its name (in Network Settings)"));
    hidden->callback([](Fl_Widget *, void *) {
      close_popup();
      fld::spawn({"fl-network"});
    });
  }
  Fl_Button *settings = new Fl_Button(W - 160, H - 38, 150, 28, _("Network Settings"));
  settings->labelsize(12);
  settings->callback([](Fl_Widget *, void *) {
    close_popup();
    fld::spawn({"fl-network"});
  });
  pop->end();
}

PanelPopup *open_network_popup() {
  PanelPopup *p = PanelPopup::open_popup();
  return p && g_button && PanelPopup::open_for() == g_button ? p : nullptr;
}

// rebuilds the open popup, keeping the list's scroll position and the
// keyboard focus on the same network
void network_popup_open(Fl_Widget *w);

// opens the popup again at the size its content needs now
void reopen_cb(void *) {
  if (!PanelPopup::open_popup() && g_button) network_popup_open(g_button);
}

void rebuild_cb(void *) {
  PanelPopup *pop = open_network_popup();
  if (!pop) return;
  // much more to show than room (networking or Wi-Fi was just turned on):
  // the popup is placed when it opens, so it opens again, taller
  {
    const nm::State &st = nm::Client::get().state();
    bool have_wifi = false;
    collect_rows(st, have_wifi);
    if (popup_height(st, have_wifi) > pop->h() + RH) {
      pop->hide();
      Fl::add_timeout(0.05, reopen_cb);
      return;
    }
  }
  int top = g_list ? g_list->yposition() : 0;
  std::string focused;
  for (Fl_Widget *f = Fl::focus(); f; f = f->parent())
    if (auto *row = dynamic_cast<NetRow *>(f)) {
      focused = row->label() ? row->label() : "";
      break;
    }
  g_list = nullptr;
  pop->clear(); // deletes the widgets; g_rows is rebuilt below
  build(pop);
  if (g_list) {
    int max = std::max(0, g_list->scrollbar.visible() ? (int)g_list->scrollbar.maximum() : 0);
    g_list->scroll_to(0, std::min(top, max));
    if (!focused.empty())
      for (int i = 0; i < g_list->children(); i++)
        if (auto *row = dynamic_cast<NetRow *>(g_list->child(i)))
          if (row->label() && focused == row->label()) row->take_focus();
  }
  pop->redraw();
}

} // namespace

void network_popup_refresh() {
  if (!open_network_popup()) return;
  // NetworkManager sends changes in bursts (a scan, a connection coming up):
  // one rebuild for each burst, and never from inside a widget's callback
  Fl::remove_timeout(rebuild_cb);
  Fl::add_timeout(0.15, rebuild_cb);
}

void network_popup(Fl_Widget *w) {
  if (panel_popup_toggle(w)) return; // open: a second click closes it
  network_popup_open(w);
}

namespace {
void network_popup_open(Fl_Widget *w) {
  nm::Client &c = nm::Client::get();
  const nm::State &st = c.state();
  if (!st.running) {
    fld::spawn({"fl-network"});
    return;
  }
  g_button = w;
  // fresh results: they fill the list while it is open
  if (st.networking && st.wireless)
    for (auto &d : st.devices)
      if (d.type == "wifi" && d.managed) c.scan(d.path);
  bool have_wifi = false;
  collect_rows(st, have_wifi);
  PanelPopup pop(W, popup_height(st, have_wifi));
  pop.end();
  build(&pop);
  pop.popup_at(w, false);
  Fl::remove_timeout(rebuild_cb);
  g_list = nullptr;
}
} // namespace

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
