#include "network_ui.h"
#include "nmclient.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Output.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multiline_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>

#include "fld/menus.h"
#include "fld/file_dialog.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

namespace nm {

static void show_error(const std::string &what, const std::string &err) {
  if (!err.empty()) fl_alert("%s:\n%s", what.c_str(), err.c_str());
}

static const char *device_icon(const Device &d) {
  if (d.type == "wifi") return d.connected ? "network-wireless" : "network-wireless-offline";
  if (d.type == "ethernet") return d.connected ? "network-wired" : "network-wired-disconnected";
  if (d.type == "modem") return "network-cellular";
  return "network-workgroup";
}

static std::string device_title(const Device &d) {
  std::string t = d.type == "wifi" ? "Wi-Fi" : d.type == "ethernet" ? "Wired" : d.type_description;
  if (t.empty()) t = d.type;
  t[0] = (char)toupper((unsigned char)t[0]);
  return t + " (" + d.iface + ")";
}

// ---- Wi-Fi network list ------------------------------------------------------

class ApList : public Fl_Group {
public:
  std::vector<AccessPoint> aps;
  int sel = -1, top = 0;
  static const int ROW = 40;
  Fl_Scrollbar *sb;
  std::function<void(const AccessPoint &)> on_activate;
  ApList(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    box(FL_FLAT_BOX);
    sb = new Fl_Scrollbar(x + w - 14, y, 14, h);
    sb->type(FL_VERTICAL);
    sb->callback([](Fl_Widget *s, void *d) {
      ((ApList *)d)->top = ((Fl_Scrollbar *)s)->value();
      ((ApList *)d)->redraw();
    }, this);
    end();
  }
  void set(const std::vector<AccessPoint> &v) {
    std::string keep = sel >= 0 && sel < (int)aps.size() ? aps[sel].ssid : "";
    aps = v;
    sel = -1;
    for (size_t i = 0; i < aps.size(); i++) if (aps[i].ssid == keep) sel = (int)i;
    update_sb();
    redraw();
  }
  const AccessPoint *selected() const { return sel >= 0 && sel < (int)aps.size() ? &aps[sel] : nullptr; }
  int rows() const { return std::max(1, h() / ROW); }
  void update_sb() {
    top = std::max(0, std::min(top, (int)aps.size() - rows()));
    sb->value(top, rows(), 0, std::max((int)aps.size(), rows()));
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    sb->resize(X + W - 14, Y, 14, H);
    update_sb();
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_push_clip(x(), y(), w(), h());
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    if (aps.empty()) {
      fl_font(FL_HELVETICA, 13);
      fl_color(p.fg_dim);
      fl_draw("No networks found", x(), y(), w() - 14, h(), FL_ALIGN_CENTER);
    }
    for (int i = top; i < (int)aps.size() && (i - top) * ROW < h(); i++) {
      const AccessPoint &a = aps[i];
      int ry = y() + (i - top) * ROW;
      if (i == sel) {
        fl_color(p.selection);
        fl_rectf(x(), ry, w() - 14, ROW);
      }
      Fl_Color fg = i == sel ? p.accent_fg : p.fg;
      if (Fl_Image *ic = fld::icon_first({signal_icon(a.strength), "network-wireless"}, 24)) ic->draw(x() + 10, ry + 8);
      fl_font(a.active ? FL_HELVETICA_BOLD : FL_HELVETICA, 14);
      fl_color(fg);
      fl_draw(fld::escape_at(a.ssid).c_str(), x() + 44, ry, w() - 260, ROW, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      fl_font(FL_HELVETICA, 12);
      fl_color(i == sel ? p.accent_fg : p.fg_dim);
      std::string right = std::string(a.active ? "Connected   " : "") + (a.secure ? "🔒 " + a.security : "Open") +
                          "   " + std::to_string(a.strength) + "%";
      fl_draw(right.c_str(), x(), ry, w() - 24, ROW, FL_ALIGN_RIGHT);
      fl_color(p.border);
      fl_xyline(x(), ry + ROW - 1, x() + w() - 14);
    }
    fl_pop_clip();
    draw_child(*sb);
  }
  int handle(int e) override {
    if (Fl::event_inside(sb) && (e == FL_PUSH || e == FL_DRAG || e == FL_RELEASE)) return Fl_Group::handle(e);
    if (e == FL_PUSH) {
      int i = top + (Fl::event_y() - y()) / ROW;
      if (i < (int)aps.size()) {
        sel = i;
        redraw();
        if (Fl::event_clicks() && on_activate) on_activate(aps[i]);
        do_callback();
      }
      return 1;
    }
    if (e == FL_MOUSEWHEEL) {
      top = std::max(0, std::min(top + Fl::event_dy(), (int)aps.size() - rows()));
      update_sb();
      redraw();
      return 1;
    }
    return Fl_Group::handle(e);
  }
};

// ---- dialogs ---------------------------------------------------------------------

std::string ask_wifi_password(const std::string &ssid, bool retry) {
  Fl_Double_Window w(400, 170, "Wi-Fi Password");
  // own app id: fl-panel's windows would otherwise be taken for the panel
  w.xclass("fl-network");
  Fl_Box *ic = new Fl_Box(16, 16, 40, 40);
  ic->image(fld::icon_first({"network-wireless-encrypted", "network-wireless", "dialog-password"}, 40));
  std::string msg = (retry ? "The password was not accepted.\n" : "") +
                    std::string("Enter the password for \"") + ssid + "\":";
  Fl_Box *m = new Fl_Box(68, 14, 316, 48);
  m->copy_label(fld::escape_at(msg).c_str());
  m->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  Fl_Secret_Input *pw = new Fl_Secret_Input(68, 70, 316, 30);
  Fl_Check_Button *show = new Fl_Check_Button(68, 102, 160, 24, "Show password");
  show->callback([](Fl_Widget *b, void *d) {
    Fl_Secret_Input *in = (Fl_Secret_Input *)d;
    in->input_type(((Fl_Check_Button *)b)->value() ? FL_NORMAL_INPUT : FL_SECRET_INPUT);
    in->redraw();
  }, pw);
  Fl_Button *cancel = new Fl_Button(176, 130, 100, 30, "Cancel");
  Fl_Return_Button *ok = new Fl_Return_Button(284, 130, 100, 30, "Connect");
  static int result;
  result = 0;
  cancel->callback([](Fl_Widget *b, void *) { result = 0; b->window()->hide(); });
  ok->callback([](Fl_Widget *b, void *) { result = 1; b->window()->hide(); });
  w.end();
  w.set_modal();
  w.show();
  pw->take_focus();
  while (w.shown()) Fl::wait();
  return result ? std::string(pw->value()) : "";
}

struct Editor {
  Settings s;
  Fl_Double_Window *win;
  Fl_Input *name, *iface, *ssid;
  Fl_Check_Button *autoconnect, *all_users, *hidden, *ignore4, *ignore6;
  Fl_Choice *security, *m4, *m6;
  Fl_Secret_Input *psk;
  Fl_Multiline_Input *addr4, *addr6;
  Fl_Input *gw4, *dns4, *gw6, *dns6;
  // WireGuard
  Fl_Secret_Input *wg_key = nullptr;
  Fl_Output *wg_pub = nullptr;
  Fl_Int_Input *wg_port = nullptr, *wg_mtu = nullptr;
  Fl_Hold_Browser *wg_peers = nullptr;
  std::vector<WgPeer> peers;
  std::string pub_s;
  // VPN plugins
  Fl_Input *vpn_user = nullptr;
  Fl_Secret_Input *vpn_pw = nullptr;
  Fl_Multiline_Input *vpn_data = nullptr;
  bool saved = false;
};

static std::string vpn_name(const std::string &service) {
  for (auto &p : vpn_plugins())
    if (p.service == service) return p.name;
  std::string s = service;
  size_t dot = s.rfind('.');
  return dot == std::string::npos ? s : s.substr(dot + 1);
}

// ---- WireGuard peers ---------------------------------------------------------------

static void fill_peers(Editor *e) {
  e->wg_peers->clear();
  for (auto &p : e->peers) {
    std::string line = p.public_key.substr(0, 12) + "…\t" + (p.endpoint.empty() ? "(no endpoint)" : p.endpoint) +
                       "\t" + p.allowed_ips;
    e->wg_peers->add(fld::escape_at(line).c_str());
  }
}

// edits one peer; false when cancelled
static bool peer_dialog(WgPeer &p, bool add) {
  Fl_Double_Window w(500, 290, add ? "Add Peer" : "Edit Peer");
  w.xclass("fl-network");
  const int LX = 150, FW = 330;
  Fl_Input *key = new Fl_Input(LX, 16, FW, 28, "Public key:");
  key->value(p.public_key.c_str());
  Fl_Input *ep = new Fl_Input(LX, 52, FW, 28, "Endpoint:");
  ep->value(p.endpoint.c_str());
  ep->tooltip("host:port of the peer, e.g. vpn.example.com:51820 (empty: it connects to you)");
  Fl_Input *ips = new Fl_Input(LX, 88, FW, 28, "Allowed IPs:");
  ips->value(p.allowed_ips.c_str());
  ips->tooltip("Addresses routed to this peer, separated by commas; 0.0.0.0/0, ::/0 sends all traffic through it");
  Fl_Secret_Input *psk = new Fl_Secret_Input(LX, 124, FW, 28, "Pre-shared key:");
  psk->value(p.preshared_key.c_str());
  psk->tooltip("Optional extra key, the same on both sides");
  Fl_Int_Input *ka = new Fl_Int_Input(LX, 160, 90, 28, "Keepalive (s):");
  if (p.keepalive) ka->value(std::to_string(p.keepalive).c_str());
  ka->tooltip("Send a packet every N seconds to keep NAT mappings open (25 is common; empty = off)");
  Fl_Button *cancel = new Fl_Button(500 - 228, 244, 104, 30, "Cancel");
  Fl_Return_Button *ok = new Fl_Return_Button(500 - 116, 244, 104, 30, "OK");
  static int result;
  result = 0;
  cancel->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  ok->callback([](Fl_Widget *b, void *) { result = 1; b->window()->hide(); });
  w.end();
  w.set_modal();
  w.show();
  for (;;) {
    while (w.shown()) Fl::wait();
    if (!result) return false;
    std::string k = fld::trim(key->value()), sk = fld::trim(psk->value());
    if (!valid_wg_key(k)) {
      fl_alert("The public key is not a WireGuard key (44 characters of base64).");
    } else if (!sk.empty() && !valid_wg_key(sk)) {
      fl_alert("The pre-shared key is not a WireGuard key.");
    } else {
      p.public_key = k;
      p.preshared_key = sk;
      p.endpoint = fld::trim(ep->value());
      p.allowed_ips = fld::trim(ips->value());
      p.keepalive = atoi(ka->value());
      return true;
    }
    result = 0;
    w.show();
  }
}

static void update_public_key(Editor *e) {
  std::string k = fld::trim(e->wg_key->value());
  e->pub_s = k.empty() ? (e->s.wg_has_private_key ? "(the stored key is not readable)" : "")
                       : valid_wg_key(k) ? wg_public_key(k) : "(not a valid private key)";
  if (e->pub_s.empty() && !k.empty() && !wg_keys_supported()) e->pub_s = "(cannot be computed here)";
  e->wg_pub->value(e->pub_s.c_str());
}

static const char *k4methods[] = {"auto", "manual", "link-local", "shared", "disabled"};
static const char *k4labels[] = {"Automatic (DHCP)", "Manual", "Link-local only", "Shared to other computers", "Disabled"};
static const char *k6methods[] = {"auto", "dhcp", "manual", "link-local", "shared", "ignore", "disabled"};
static const char *k6labels[] = {"Automatic", "Automatic, DHCP only", "Manual", "Link-local only",
                                 "Shared to other computers", "Ignore", "Disabled"};

static void update_ip_fields(Editor *e) {
  bool man4 = std::string(k4methods[std::max(0, e->m4->value())]) == "manual";
  bool man6 = std::string(k6methods[std::max(0, e->m6->value())]) == "manual";
  for (Fl_Widget *w : {(Fl_Widget *)e->addr4, (Fl_Widget *)e->gw4}) man4 ? w->activate() : w->deactivate();
  for (Fl_Widget *w : {(Fl_Widget *)e->addr6, (Fl_Widget *)e->gw6}) man6 ? w->activate() : w->deactivate();
}

static void editor_save(Fl_Widget *, void *data) {
  Editor *e = (Editor *)data;
  Settings &s = e->s;
  s.id = fld::trim(e->name->value());
  s.iface = fld::trim(e->iface->value());
  s.autoconnect = e->autoconnect->value();
  s.all_users = e->all_users->value();
  if (s.type == "802-11-wireless") {
    s.ssid = e->ssid->value();
    s.hidden = e->hidden->value();
    const char *sec[] = {"none", "wpa-psk", "sae"};
    s.security = sec[std::max(0, e->security->value())];
    s.psk = e->psk->value();
    if (s.id.empty()) s.id = s.ssid;
  }
  if (s.type == "wireguard") {
    s.wg_private_key = fld::trim(e->wg_key->value());
    s.wg_listen_port = atoi(e->wg_port->value());
    s.wg_mtu = atoi(e->wg_mtu->value());
    s.wg_peers = e->peers;
    if (s.id.empty()) s.id = s.iface.empty() ? "WireGuard" : s.iface;
  } else if (s.type == "vpn") {
    s.vpn_user = fld::trim(e->vpn_user->value());
    s.vpn_password = e->vpn_pw->value();
    s.vpn_data = parse_kv(e->vpn_data->value());
  }
  s.ip4_method = k4methods[std::max(0, e->m4->value())];
  s.ip4_addresses = e->addr4->value();
  s.ip4_gateway = fld::trim(e->gw4->value());
  s.ip4_dns = e->dns4->value();
  s.ip4_ignore_auto_dns = e->ignore4->value();
  s.ip6_method = k6methods[std::max(0, e->m6->value())];
  s.ip6_addresses = e->addr6->value();
  s.ip6_gateway = fld::trim(e->gw6->value());
  s.ip6_dns = e->dns6->value();
  s.ip6_ignore_auto_dns = e->ignore6->value();
  e->win->deactivate();
  Client::get().save_settings(s, [e](const std::string &err) {
    e->win->activate();
    if (!err.empty()) {
      show_error("The connection could not be saved", err);
      return;
    }
    e->saved = true;
    e->win->hide();
  });
}

static void build_editor(Editor *e) {
  const Settings &s = e->s;
  const int W = 520, H = 470;
  std::string title = s.uuid.empty() ? "New Connection" : "Edit " + s.id;
  e->win = new Fl_Double_Window(W, H);
  e->win->copy_label(title.c_str());
  Fl_Tabs *tabs = new Fl_Tabs(10, 10, W - 20, H - 70);
  const int TY = 44, LX = 160, FW = W - 190;

  Fl_Group *gen = new Fl_Group(10, 40, W - 20, H - 100, "General");
  e->name = new Fl_Input(LX, TY + 10, FW, 28, "Name:");
  e->name->value(s.id.c_str());
  e->iface = new Fl_Input(LX, TY + 46, FW, 28, "Device:");
  e->iface->value(s.iface.c_str());
  e->iface->tooltip("Restrict to an interface name, e.g. eth0 (empty = any)");
  e->autoconnect = new Fl_Check_Button(LX, TY + 84, FW, 26, "Connect automatically");
  e->autoconnect->value(s.autoconnect);
  e->all_users = new Fl_Check_Button(LX, TY + 112, FW, 26, "Available to all users");
  e->all_users->value(s.all_users);
  gen->end();

  if (s.type == "802-11-wireless") {
    Fl_Group *wifi = new Fl_Group(10, 40, W - 20, H - 100, "Wi-Fi");
    e->ssid = new Fl_Input(LX, TY + 10, FW, 28, "Network name (SSID):");
    e->ssid->value(s.ssid.c_str());
    e->hidden = new Fl_Check_Button(LX, TY + 44, FW, 26, "Hidden network");
    e->hidden->value(s.hidden);
    e->security = new Fl_Choice(LX, TY + 78, FW, 28, "Security:");
    e->security->add("None");
    e->security->add("WPA/WPA2 Personal");
    e->security->add("WPA3 Personal");
    e->security->value(s.security == "wpa-psk" ? 1 : s.security == "sae" ? 2 : 0);
    e->psk = new Fl_Secret_Input(LX, TY + 114, FW, 28, "Password:");
    e->psk->tooltip(s.uuid.empty() ? "" : "Leave empty to keep the saved password");
    Fl_Box *note = new Fl_Box(LX, TY + 146, FW, 40);
    if (!s.uuid.empty()) note->label("Leave the password empty to keep the saved one.");
    note->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP | FL_ALIGN_TOP);
    note->labelsize(12);
    note->labelcolor(fld::palette().fg_dim);
    if (s.security != "none" && s.security != "wpa-psk" && s.security != "sae") {
      note->label("This network uses a security type the editor cannot change (e.g. 802.1X).");
      e->security->deactivate();
      e->psk->deactivate();
    }
    wifi->end();
  } else {
    e->ssid = nullptr;
    e->hidden = nullptr;
    e->security = nullptr;
    e->psk = nullptr;
  }

  if (s.type == "wireguard") {
    Fl_Group *wg = new Fl_Group(10, 40, W - 20, H - 100, "WireGuard");
    e->wg_key = new Fl_Secret_Input(LX, TY + 10, FW - 96, 28, "Private key:");
    e->wg_key->value(s.wg_private_key.c_str());
    e->wg_key->when(FL_WHEN_CHANGED);
    e->wg_key->callback([](Fl_Widget *, void *d) { update_public_key((Editor *)d); }, e);
    Fl_Button *gen = new Fl_Button(LX + FW - 90, TY + 10, 90, 28, "Generate");
    gen->tooltip("Make a new key pair; give the public key to the other side");
    gen->callback([](Fl_Widget *, void *d) {
      Editor *ed = (Editor *)d;
      if (!fld::trim(ed->wg_key->value()).empty() &&
          fl_choice("Replace the private key? The other side then needs the new public key.", "Cancel", "Replace",
                    nullptr) != 1)
        return;
      ed->wg_key->value(wg_generate_private_key().c_str());
      update_public_key(ed);
    }, e);
    if (!wg_keys_supported()) gen->deactivate();
    e->wg_pub = new Fl_Output(LX, TY + 44, FW, 28, "Public key:");
    e->wg_pub->tooltip("Give this to the other side (it is computed from the private key)");
    e->wg_port = new Fl_Int_Input(LX, TY + 78, 90, 28, "Listen port:");
    if (s.wg_listen_port) e->wg_port->value(std::to_string(s.wg_listen_port).c_str());
    e->wg_port->tooltip("Empty: a random port");
    e->wg_mtu = new Fl_Int_Input(LX + FW - 90, TY + 78, 90, 28, "MTU:");
    if (s.wg_mtu) e->wg_mtu->value(std::to_string(s.wg_mtu).c_str());
    e->wg_mtu->tooltip("Empty: automatic");
    e->wg_peers = new Fl_Hold_Browser(LX, TY + 116, FW, H - 100 - 116 - 50, "Peers:");
    e->wg_peers->align(FL_ALIGN_LEFT_TOP);
    static int pw[] = {110, 150, 0};
    e->wg_peers->column_widths(pw);
    e->wg_peers->column_char('\t');
    e->wg_peers->textsize(12);
    e->wg_peers->callback([](Fl_Widget *w, void *d) {
      Editor *ed = (Editor *)d;
      int i = ((Fl_Hold_Browser *)w)->value() - 1;
      if (Fl::event_clicks() && i >= 0 && peer_dialog(ed->peers[(size_t)i], false)) fill_peers(ed);
    }, e);
    int by = TY + 116 + (H - 100 - 116 - 50) + 6;
    Fl_Button *add = new Fl_Button(LX, by, 90, 28, "Add…");
    add->callback([](Fl_Widget *, void *d) {
      Editor *ed = (Editor *)d;
      WgPeer p;
      p.allowed_ips = "0.0.0.0/0, ::/0";
      if (peer_dialog(p, true)) {
        ed->peers.push_back(p);
        fill_peers(ed);
      }
    }, e);
    Fl_Button *edit = new Fl_Button(LX + 96, by, 90, 28, "Edit…");
    edit->callback([](Fl_Widget *, void *d) {
      Editor *ed = (Editor *)d;
      int i = ed->wg_peers->value() - 1;
      if (i >= 0 && peer_dialog(ed->peers[(size_t)i], false)) fill_peers(ed);
    }, e);
    Fl_Button *rm = new Fl_Button(LX + 192, by, 90, 28, "Remove");
    rm->callback([](Fl_Widget *, void *d) {
      Editor *ed = (Editor *)d;
      int i = ed->wg_peers->value() - 1;
      if (i < 0) return;
      ed->peers.erase(ed->peers.begin() + i);
      fill_peers(ed);
    }, e);
    wg->end();
    e->peers = s.wg_peers;
    fill_peers(e);
    update_public_key(e);
  } else if (s.type == "vpn") {
    Fl_Group *vg = new Fl_Group(10, 40, W - 20, H - 100, "VPN");
    Fl_Box *kind = new Fl_Box(LX, TY + 10, FW, 28);
    kind->copy_label(fld::escape_at(vpn_name(s.vpn_service)).c_str());
    kind->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    kind->labelfont(FL_HELVETICA_BOLD);
    Fl_Box *kl = new Fl_Box(10, TY + 10, LX - 14, 28, "Type:");
    kl->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
    e->vpn_user = new Fl_Input(LX, TY + 44, FW, 28, "User name:");
    e->vpn_user->value(s.vpn_user.c_str());
    e->vpn_pw = new Fl_Secret_Input(LX, TY + 78, FW, 28, "Password:");
    e->vpn_pw->value(s.vpn_password.c_str());
    e->vpn_pw->tooltip("Stored by NetworkManager; empty: asked when connecting");
    e->vpn_data = new Fl_Multiline_Input(LX, TY + 116, FW, H - 100 - 116 - 46, "Settings:");
    e->vpn_data->value(format_kv(s.vpn_data).c_str());
    e->vpn_data->textfont(FL_COURIER);
    e->vpn_data->textsize(12);
    e->vpn_data->tooltip("The plugin's settings, one \"key = value\" per line (e.g. remote = vpn.example.com)");
    Fl_Box *note = new Fl_Box(LX, H - 100 + 40 - 40, FW, 36,
                              "The keys depend on the VPN type; importing the provider's file fills them in.");
    note->labelsize(11);
    note->labelcolor(fld::palette().fg_dim);
    note->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
    vg->end();
  }

  auto ip_tab = [&](bool v6) {
    Fl_Group *g = new Fl_Group(10, 40, W - 20, H - 100, v6 ? "IPv6" : "IPv4");
    Fl_Choice *m = new Fl_Choice(LX, TY + 10, FW, 28, "Method:");
    const char **labels = v6 ? k6labels : k4labels;
    const char **methods = v6 ? k6methods : k4methods;
    int n = v6 ? 7 : 5, sel = 0;
    const std::string &cur = v6 ? s.ip6_method : s.ip4_method;
    for (int i = 0; i < n; i++) {
      m->add(labels[i]);
      if (cur == methods[i]) sel = i;
    }
    m->value(sel);
    m->callback([](Fl_Widget *, void *d) { update_ip_fields((Editor *)d); }, e);
    Fl_Multiline_Input *addr = new Fl_Multiline_Input(LX, TY + 46, FW, 70, "Addresses:");
    addr->tooltip(v6 ? "One per line, e.g. 2001:db8::5/64" : "One per line, e.g. 192.168.1.10/24");
    addr->value((v6 ? s.ip6_addresses : s.ip4_addresses).c_str());
    Fl_Input *gw = new Fl_Input(LX, TY + 124, FW, 28, "Gateway:");
    gw->value((v6 ? s.ip6_gateway : s.ip4_gateway).c_str());
    Fl_Input *dns = new Fl_Input(LX, TY + 160, FW, 28, "DNS servers:");
    dns->tooltip("Separated by spaces or commas");
    dns->value((v6 ? s.ip6_dns : s.ip4_dns).c_str());
    Fl_Check_Button *ign = new Fl_Check_Button(LX, TY + 194, FW, 26, "Use only these DNS servers");
    ign->value(v6 ? s.ip6_ignore_auto_dns : s.ip4_ignore_auto_dns);
    g->end();
    if (v6) { e->m6 = m; e->addr6 = addr; e->gw6 = gw; e->dns6 = dns; e->ignore6 = ign; }
    else { e->m4 = m; e->addr4 = addr; e->gw4 = gw; e->dns4 = dns; e->ignore4 = ign; }
  };
  ip_tab(false);
  ip_tab(true);
  tabs->end();
  Fl_Button *cancel = new Fl_Button(W - 230, H - 48, 104, 32, "Cancel");
  cancel->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  Fl_Return_Button *save = new Fl_Return_Button(W - 118, H - 48, 104, 32, "Save");
  save->callback(editor_save, e);
  e->win->end();
  e->win->set_modal();
  update_ip_fields(e);
}

void edit_connection(const std::string &uuid, const std::string &type) {
  auto open = [](const Settings &s) {
    Editor *e = new Editor;
    e->s = s;
    build_editor(e);
    e->win->show();
    while (e->win->shown()) Fl::wait();
    Fl::delete_widget(e->win);
    delete e;
  };
  if (uuid.empty()) {
    Settings s;
    s.type = type.empty() ? "802-3-ethernet" : type;
    s.id = s.type == "802-11-wireless" ? "" : "Wired connection";
    if (s.type == "wireguard") {
      s.id = "WireGuard";
      s.iface = "wg0";
      s.autoconnect = false;
      s.ip4_method = "manual";
      s.ip6_method = "disabled";
      s.wg_private_key = wg_generate_private_key();
    } else if (fld::starts_with(s.type, "vpn:")) {
      s.vpn_service = s.type.substr(4);
      s.type = "vpn";
      s.id = vpn_name(s.vpn_service);
      s.autoconnect = false;
    }
    open(s);
    return;
  }
  Client::get().get_settings(uuid, [open](const Settings &s, const std::string &err) {
    if (!err.empty()) { show_error("Cannot read the connection", err); return; }
    open(s);
  });
}

// ---- the view -----------------------------------------------------------------------

NetworkView::NetworkView(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
  const fld::Palette &p = fld::palette();
  box(FL_FLAT_BOX);
  color(p.bg);
  networking_ = new Fl_Check_Button(X + 10, Y + 8, 130, 26, "Networking");
  networking_->callback([](Fl_Widget *w, void *) {
    Client::get().set_networking(((Fl_Check_Button *)w)->value(),
                                 [](const std::string &e) { show_error("Could not change networking", e); });
  });
  wireless_ = new Fl_Check_Button(X + 146, Y + 8, 90, 26, "Wi-Fi");
  wireless_->callback([](Fl_Widget *w, void *) {
    Client::get().set_wireless(((Fl_Check_Button *)w)->value(),
                               [](const std::string &e) { show_error("Could not change Wi-Fi", e); });
  });
  summary_ = new Fl_Box(X + 240, Y + 8, W - 250, 26);
  summary_->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT | FL_ALIGN_CLIP);
  summary_->labelcolor(p.fg_dim);
  summary_->labelsize(12);
  sidebar_ = new Fl_Hold_Browser(X, Y + 42, 210, H - 42);
  sidebar_->textsize(13);
  sidebar_->selection_color(p.accent);
  sidebar_->callback(sidebar_cb, this);

  // device page
  dev_page_ = new Fl_Group(X + 210, Y + 42, W - 210, H - 42);
  dev_page_->box(FL_FLAT_BOX);
  dev_page_->color(p.bg);
  dev_icon_ = new Fl_Box(0, 0, 48, 48);
  dev_title_ = new Fl_Box(0, 0, 10, 26);
  dev_title_->labelfont(FL_HELVETICA_BOLD);
  dev_title_->labelsize(17);
  dev_title_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  dev_state_ = new Fl_Box(0, 0, 10, 20);
  dev_state_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  dev_state_->labelcolor(p.fg_dim);
  dev_toggle_ = new Fl_Button(0, 0, 120, 30, "Disconnect");
  dev_toggle_->callback(toggle_device_cb, this);
  dev_edit_ = new Fl_Button(0, 0, 120, 30, "Settings…");
  dev_edit_->callback(edit_device_cb, this);
  dev_info_ = new Fl_Box(0, 0, 10, 10);
  dev_info_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP);
  dev_info_->labelfont(FL_COURIER);
  dev_info_->labelsize(13);
  aps_ = new ApList(0, 0, 10, 10);
  aps_->on_activate = [this](const AccessPoint &) { wifi_connect_cb(nullptr, this); };
  wifi_connect_ = new Fl_Button(0, 0, 110, 30, "Connect");
  wifi_connect_->callback(wifi_connect_cb, this);
  wifi_hidden_ = new Fl_Button(0, 0, 150, 30, "Hidden Network…");
  wifi_hidden_->callback(wifi_hidden_cb, this);
  wifi_scan_ = new Fl_Button(0, 0, 90, 30, "Scan");
  wifi_scan_->callback([](Fl_Widget *, void *d) { Client::get().scan(((NetworkView *)d)->page_); }, this);
  dev_page_->end();

  // connections page
  conn_page_ = new Fl_Group(X + 210, Y + 42, W - 210, H - 42);
  conn_page_->box(FL_FLAT_BOX);
  conn_page_->color(p.bg);
  conns_ = new Fl_Hold_Browser(0, 0, 10, 10);
  conns_->textsize(13);
  conns_->selection_color(p.accent);
  conns_->callback([](Fl_Widget *, void *d) {
    if (Fl::event_clicks()) conn_edit_cb(nullptr, d);
    ((NetworkView *)d)->fill_connections_page();
  }, this);
  static int col_widths[] = {230, 130, 0};
  conns_->column_widths(col_widths);
  conns_->column_char('\t');
  c_toggle_ = new Fl_Button(0, 0, 110, 30, "Connect");
  c_toggle_->callback(conn_toggle_cb, this);
  c_edit_ = new Fl_Button(0, 0, 90, 30, "Edit…");
  c_edit_->callback(conn_edit_cb, this);
  c_delete_ = new Fl_Button(0, 0, 90, 30, "Delete");
  c_delete_->callback(conn_delete_cb, this);
  // one "Add" button with a menu: wired, Wi-Fi, WireGuard, VPN plugins, import
  c_add_ = new Fl_Button(0, 0, 120, 30, "Add…");
  c_add_->callback(conn_add_vpn_cb, this);
  conn_page_->end();
  conn_page_->hide();
  // right-click menus: the same as the buttons of each page
  fld::set_context_menu(aps_, [this](fld::Menu &m) {
    m.button(wifi_connect_).divider().button(wifi_scan_).button(wifi_hidden_);
  });
  fld::set_context_menu(conns_, [this](fld::Menu &m) {
    m.button(c_toggle_).button(c_edit_).button(c_delete_).divider().button(c_add_);
  });
  fld::set_context_menu(sidebar_, [this](fld::Menu &m) {
    if (dev_page_->visible()) m.button(dev_toggle_).button(dev_edit_);
  });
  end();
  resizable(dev_page_);
  page_ = "";
  layout_children();
  refresh();
}

void NetworkView::layout_children() {
  int X = x() + 210, Y = y() + 42, W = w() - 210, H = h() - 42;
  sidebar_->resize(x(), Y, 210, H);
  summary_->resize(x() + 240, y() + 8, w() - 250, 26);
  dev_page_->resize(X, Y, W, H);
  conn_page_->resize(X, Y, W, H);
  int px = X + 16, pw = W - 32;
  dev_icon_->resize(px, Y + 12, 48, 48);
  dev_title_->resize(px + 60, Y + 12, pw - 330, 26);
  dev_state_->resize(px + 60, Y + 38, pw - 330, 20);
  dev_edit_->resize(X + W - 16 - 120, Y + 20, 120, 30);
  dev_toggle_->resize(X + W - 16 - 248, Y + 20, 120, 30);
  bool wifi = false;
  if (const Device *d = Client::get().state().device(page_)) wifi = d->type == "wifi";
  if (wifi) {
    dev_info_->resize(px, Y + 72, pw, 70);
    aps_->resize(px, Y + 150, pw, H - 150 - 52);
    wifi_connect_->resize(px, Y + H - 44, 110, 30);
    wifi_hidden_->resize(px + 118, Y + H - 44, 150, 30);
    wifi_scan_->resize(px + 276, Y + H - 44, 90, 30);
  } else {
    dev_info_->resize(px, Y + 72, pw, H - 80);
  }
  conns_->resize(px, Y + 12, pw, H - 70);
  int by = Y + H - 44;
  c_toggle_->resize(px, by, 110, 30);
  c_edit_->resize(px + 116, by, 90, 30);
  c_delete_->resize(px + 212, by, 90, 30);
  c_add_->resize(X + W - 16 - 120, by, 120, 30);
}

void NetworkView::resize(int X, int Y, int W, int H) {
  Fl_Widget::resize(X, Y, W, H);
  layout_children();
}

void NetworkView::refresh() {
  const State &st = Client::get().state();
  networking_->value(st.networking);
  wireless_->value(st.wireless);
  bool have_wifi = false;
  for (auto &d : st.devices) if (d.type == "wifi") have_wifi = true;
  if (have_wifi && st.wireless_hw) wireless_->show(); else wireless_->hide();
  if (!st.running) summary_s_ = "NetworkManager is not running";
  else if (st.primary_id.empty()) summary_s_ = "Not connected";
  else summary_s_ = "Connected: " + st.primary_id + (st.connectivity == "full" || st.connectivity == "unknown" ? "" :
                                                     st.connectivity == "portal" ? " (sign-in needed)" : " (limited)");
  summary_s_ = fld::escape_at(summary_s_);
  summary_->label(summary_s_.c_str());

  // sidebar: devices (no loopback), then "All connections"
  std::vector<std::string> keys;
  std::string keep = page_;
  sidebar_->clear();
  for (auto &d : st.devices) {
    if (d.type == "loopback" || !d.managed) continue;
    keys.push_back(d.path);
    sidebar_->add(("  " + fld::escape_at(device_title(d))).c_str());
    sidebar_->icon(sidebar_->size(), fld::icon_first({device_icon(d), "network-wired", "fl-network"}, 20));
  }
  keys.push_back("*");
  sidebar_->add("  All Connections");
  sidebar_->icon(sidebar_->size(), fld::icon_first({"network-workgroup", "preferences-system-network"}, 20));
  sidebar_keys_ = keys;
  if (!user_chose_ || std::find(keys.begin(), keys.end(), keep) == keys.end()) keep = keys.empty() ? "*" : keys[0];
  for (size_t i = 0; i < keys.size(); i++)
    if (keys[i] == keep) sidebar_->value((int)i + 1);
  show_page(keep);
}

void NetworkView::show_page(const std::string &key) {
  page_ = key;
  if (key == "*") {
    dev_page_->hide();
    conn_page_->show();
    fill_connections_page();
  } else {
    conn_page_->hide();
    dev_page_->show();
    fill_device_page();
  }
  layout_children();
  redraw();
}

void NetworkView::fill_device_page() {
  const Device *d = Client::get().state().device(page_);
  if (!d) return;
  dev_icon_->image(fld::icon_first({device_icon(*d), "network-wired", "fl-network"}, 48));
  title_s_ = fld::escape_at(device_title(*d));
  dev_title_->label(title_s_.c_str());
  state_s_ = d->state + (d->active_id.empty() ? "" : " – " + d->active_id);
  state_s_ = fld::escape_at(state_s_);
  dev_state_->label(state_s_.c_str());
  std::string info;
  auto line = [&](const char *k, const std::string &v) {
    if (v.empty()) return;
    char buf[32];
    snprintf(buf, sizeof buf, "%-12s", k);
    info += std::string(buf) + v + "\n";
  };
  line("IPv4", fld::join(d->ip4, "  "));
  line("Gateway", d->gateway4);
  line("IPv6", fld::join(d->ip6, "  "));
  line("DNS", fld::join(d->dns, "  "));
  line("Hardware", d->hwaddr);
  if (d->speed) line("Speed", std::to_string(d->speed) + " Mb/s");
  line("Driver", d->driver);
  if (!d->managed) info += "\nThis device is not managed by NetworkManager.";
  info_s_ = fld::escape_at(info);
  dev_info_->label(info_s_.c_str());
  bool wifi = d->type == "wifi";
  dev_toggle_->label(d->connected || d->connecting ? "Disconnect" : "Connect");
  if (!d->managed || !d->available) dev_toggle_->deactivate(); else dev_toggle_->activate();
  if (d->active_uuid.empty()) dev_edit_->deactivate(); else dev_edit_->activate();
  for (Fl_Widget *w : {(Fl_Widget *)aps_, (Fl_Widget *)wifi_connect_, (Fl_Widget *)wifi_hidden_, (Fl_Widget *)wifi_scan_})
    wifi ? w->show() : w->hide();
  if (wifi) {
    aps_->set(d->aps);
    if (wifi && !d->available) wifi_connect_->deactivate(); else wifi_connect_->activate();
    if (wifi && !d->connected && d->type == "wifi") dev_toggle_->hide(); else dev_toggle_->show();
  } else {
    dev_toggle_->show();
  }
}

std::string NetworkView::selected_uuid() const {
  int line = conns_->value();
  return line > 0 && line <= (int)conn_uuids_.size() ? conn_uuids_[line - 1] : "";
}

static std::string type_label(const std::string &t) {
  if (t == "802-3-ethernet") return "Wired";
  if (t == "802-11-wireless") return "Wi-Fi";
  if (t == "vpn") return "VPN";
  if (t == "wireguard") return "WireGuard";
  if (t == "bridge") return "Bridge";
  if (t == "bond") return "Bond";
  if (t == "gsm" || t == "cdma") return "Mobile";
  return t;
}

void NetworkView::fill_connections_page() {
  const State &st = Client::get().state();
  std::string keep = selected_uuid();
  int top = conns_->topline();
  conns_->clear();
  conn_uuids_.clear();
  int sel = 0;
  for (auto &c : st.connections) {
    std::string line = (c.active ? "@b" : "") + fld::escape_at(c.id) + "\t" + type_label(c.type) + "\t" +
                       (c.active ? "Active" : "");
    conns_->add(line.c_str());
    conn_uuids_.push_back(c.uuid);
    if (c.uuid == keep) sel = (int)conn_uuids_.size();
  }
  conns_->value(sel);
  conns_->topline(top);
  const Connection *c = sel ? st.connection(conn_uuids_[sel - 1]) : nullptr;
  for (Fl_Widget *w : {(Fl_Widget *)c_edit_, (Fl_Widget *)c_delete_, (Fl_Widget *)c_toggle_})
    c ? w->activate() : w->deactivate();
  c_toggle_->label(c && c->active ? "Disconnect" : "Connect");
  if (c && c->type != "802-3-ethernet" && c->type != "802-11-wireless" && c->type != "wireguard" && c->type != "vpn")
    c_edit_->deactivate();
}

// ---- callbacks ----------------------------------------------------------------------

void NetworkView::sidebar_cb(Fl_Widget *, void *data) {
  NetworkView *v = (NetworkView *)data;
  int line = v->sidebar_->value();
  if (line > 0 && line <= (int)v->sidebar_keys_.size()) {
    v->user_chose_ = true;
    v->show_page(v->sidebar_keys_[line - 1]);
  }
}

void NetworkView::toggle_device_cb(Fl_Widget *, void *data) {
  NetworkView *v = (NetworkView *)data;
  const State &st = Client::get().state();
  const Device *d = st.device(v->page_);
  if (!d) return;
  auto err = [](const std::string &e) { show_error("The connection could not be changed", e); };
  if (d->connected || d->connecting) {
    Client::get().disconnect_device(d->path, err);
    return;
  }
  // connect: the most recently used connection that fits this device
  std::string want = d->type == "ethernet" ? "802-3-ethernet" : d->type == "wifi" ? "802-11-wireless" : "";
  for (auto &c : st.connections) {
    if (c.type != want || (!c.iface.empty() && c.iface != d->iface)) continue;
    Client::get().activate(c.uuid, d->path, err);
    return;
  }
  if (want == "802-3-ethernet") {
    if (fl_choice("There is no wired connection yet. Create one?", "Cancel", "Create", nullptr) == 1)
      edit_connection("", "802-3-ethernet");
  }
}

void NetworkView::edit_device_cb(Fl_Widget *, void *data) {
  NetworkView *v = (NetworkView *)data;
  const Device *d = Client::get().state().device(v->page_);
  if (d && !d->active_uuid.empty()) edit_connection(d->active_uuid);
}

void NetworkView::wifi_connect_cb(Fl_Widget *, void *data) {
  NetworkView *v = (NetworkView *)data;
  const AccessPoint *ap = v->aps_->selected();
  if (!ap) { fl_message("Choose a network first."); return; }
  if (ap->active) return;
  if (ap->enterprise) {
    fl_alert("\"%s\" uses enterprise (802.1X) security, which this dialog cannot set up yet.\n"
             "Use nmcli or nm-connection-editor for it.", ap->ssid.c_str());
    return;
  }
  std::string password;
  if (ap->secure && !Client::get().has_saved_wifi(ap->ssid)) {
    password = ask_wifi_password(ap->ssid);
    if (password.empty()) return;
  }
  std::string dev = v->page_, ssid = ap->ssid;
  Client::get().connect_wifi(dev, ssid, password, false,
                             [](const std::string &e) { show_error("Could not connect", e); });
}

void NetworkView::wifi_hidden_cb(Fl_Widget *, void *data) {
  NetworkView *v = (NetworkView *)data;
  const char *ssid = fl_input("Name (SSID) of the hidden network:");
  if (!ssid || !*ssid) return;
  std::string s = ssid;
  std::string pw = ask_wifi_password(s);
  Client::get().connect_wifi(v->page_, s, pw, true, [](const std::string &e) { show_error("Could not connect", e); });
}

void NetworkView::conn_add_cb(Fl_Widget *, void *type) { edit_connection("", (const char *)type); }

void import_vpn_dialog() {
  fld::FileDialogOptions o;
  o.title = "Import a VPN Configuration";
  fld::FileFilter f;
  f.name = "VPN configurations";
  f.globs = {"*.conf", "*.ovpn", "*.pcf", "*.vpnc", "*.cfg", "*.pbk"};
  fld::FileFilter all;
  all.name = "All files";
  all.globs = {"*"};
  o.filters = {f, all};
  auto r = fld::run_file_dialog(o);
  if (!r.ok || r.paths.empty()) return;
  std::string path = r.paths[0];
  Client::get().import_vpn(path, [path](const std::string &err, const std::string &uuid) {
    if (!err.empty()) {
      show_error("Could not import " + fld::basename(path), err);
      return;
    }
    // show what was imported (the connection list updates a moment later)
    Fl::add_timeout(0.3, [](void *d) {
      std::string *u = (std::string *)d;
      edit_connection(*u);
      delete u;
    }, new std::string(uuid));
  });
}

void NetworkView::conn_add_vpn_cb(Fl_Widget *w, void *) {
  static std::vector<VpnPlugin> plugins;
  static std::vector<std::string> labels, types;
  plugins = vpn_plugins();
  labels.clear();
  types.clear();
  labels.reserve(plugins.size());
  types.reserve(plugins.size());
  std::vector<Fl_Menu_Item> m = {
      {"Wired Connection…", 0, nullptr, (void *)3},
      {"Wi-Fi Connection…", 0, nullptr, (void *)4, FL_MENU_DIVIDER},
      {"WireGuard VPN…", 0, nullptr, (void *)1},
  };
  for (size_t i = 0; i < plugins.size(); i++) {
    labels.push_back(fld::escape_at(plugins[i].name) + " VPN…");
    types.push_back("vpn:" + plugins[i].service);
    m.push_back({labels.back().c_str(), 0, nullptr, (void *)(long)(100 + i)});
  }
  m.back().flags |= FL_MENU_DIVIDER;
  m.push_back({"Import VPN from File…", 0, nullptr, (void *)2});
  m.push_back({nullptr});
  const Fl_Menu_Item *r = m[0].popup(w->x(), w->y() + w->h());
  if (!r) return;
  long id = (long)r->user_data();
  if (id == 1) edit_connection("", "wireguard");
  else if (id == 2) import_vpn_dialog();
  else if (id == 3) edit_connection("", "802-3-ethernet");
  else if (id == 4) edit_connection("", "802-11-wireless");
  else if (id >= 100) edit_connection("", types[(size_t)(id - 100)]);
}

void NetworkView::conn_edit_cb(Fl_Widget *, void *data) {
  std::string uuid = ((NetworkView *)data)->selected_uuid();
  const Connection *c = Client::get().state().connection(uuid);
  if (c && (c->type == "802-3-ethernet" || c->type == "802-11-wireless")) edit_connection(uuid);
}

void NetworkView::conn_delete_cb(Fl_Widget *, void *data) {
  std::string uuid = ((NetworkView *)data)->selected_uuid();
  const Connection *c = Client::get().state().connection(uuid);
  if (!c) return;
  if (fl_choice("Delete the connection \"%s\"?", "Cancel", "Delete", nullptr, c->id.c_str()) != 1) return;
  Client::get().delete_connection(uuid, [](const std::string &e) { show_error("Could not delete", e); });
}

void NetworkView::conn_toggle_cb(Fl_Widget *, void *data) {
  std::string uuid = ((NetworkView *)data)->selected_uuid();
  const Connection *c = Client::get().state().connection(uuid);
  if (!c) return;
  auto err = [](const std::string &e) { show_error("The connection could not be changed", e); };
  if (c->active) Client::get().deactivate(uuid, err);
  else Client::get().activate(uuid, "", err);
}

} // namespace nm
