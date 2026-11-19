#include "fw_ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_ask.H>

#include <algorithm>

#include "fld/menus.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

namespace fw {

void FirewallView::done_cb(const std::string &err) {
  if (!err.empty()) fl_alert("The firewall could not be changed.\n\n%s", err.c_str());
}

FirewallView::FirewallView(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
  status_ = new Fl_Box(X, Y, W - 330, 26);
  status_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  status_->labelfont(FL_HELVETICA_BOLD);
  reload_ = new Fl_Button(X + W - 324, Y, 100, 28, "Reload");
  reload_->tooltip("Load the permanent configuration again (drops changes made only for now)");
  reload_->callback([](Fl_Widget *, void *) { Client::get().reload(done_cb); });
  to_perm_ = new Fl_Button(X + W - 218, Y, 218, 28, "Keep Current Rules");
  to_perm_->tooltip("Save the running configuration as the permanent one");
  to_perm_->callback([](Fl_Widget *, void *) { Client::get().runtime_to_permanent(done_cb); });
  default_zone_ = new Fl_Choice(X + 150, Y + 34, 200, 28, "Default zone:");
  default_zone_->callback([](Fl_Widget *w, void *d) {
    FirewallView *v = (FirewallView *)d;
    int i = ((Fl_Choice *)w)->value();
    if (v->filling_ || i < 0 || i >= (int)v->zone_names_.size()) return;
    Client::get().set_default_zone(v->zone_names_[i], done_cb);
  }, this);
  panic_ = new Fl_Check_Button(X + 370, Y + 34, 330, 28, "Panic mode: block all network traffic");
  panic_->callback([](Fl_Widget *w, void *d) {
    FirewallView *v = (FirewallView *)d;
    bool on = ((Fl_Check_Button *)w)->value();
    if (on && fl_choice("Block all network traffic of this computer?\nOpen connections break at once.",
                        "Cancel", "Block Everything", nullptr) != 1) {
      ((Fl_Check_Button *)w)->value(0);
      return;
    }
    (void)v;
    Client::get().set_panic(on, done_cb);
  }, this);
  permanent_ = new Fl_Check_Button(X, Y + 66, 420, 26, "Save changes permanently (also after a restart)");
  permanent_->value(1);
  zones_ = new Fl_Hold_Browser(X, Y + 100, 170, H - 100);
  zones_->textsize(14);
  zones_->format_char(0);
  zones_->callback([](Fl_Widget *w, void *d) {
    FirewallView *v = (FirewallView *)d;
    int i = ((Fl_Hold_Browser *)w)->value() - 1;
    if (i >= 0 && i < (int)v->zone_names_.size()) {
      v->zone_ = v->zone_names_[i];
      v->fill_zone();
    }
  }, this);
  int dx = X + 180, dw = W - 180;
  zone_info_ = new Fl_Box(dx, Y + 100, dw, 54);
  zone_info_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  zone_info_->labelsize(12);
  zone_info_->labelcolor(fld::palette().fg_dim);
  tabs_ = new Fl_Tabs(dx, Y + 158, dw, H - 158);
  int ty = Y + 188, th = H - 188;
  // services
  Fl_Group *g1 = new Fl_Group(dx, ty, dw, th, "Services");
  services_ = new Fl_Check_Browser(dx + 6, ty + 6, dw - 12, th - 12);
  services_->textsize(13);
  services_->when(FL_WHEN_CHANGED);
  services_->callback([](Fl_Widget *w, void *d) {
    FirewallView *v = (FirewallView *)d;
    Fl_Check_Browser *b = (Fl_Check_Browser *)w;
    int i = b->value() - 1;
    if (v->filling_ || i < 0 || i >= (int)v->service_names_.size() || !v->zone()) return;
    bool on = b->checked(i + 1);
    const Zone *z = v->zone();
    bool was = std::find(z->services.begin(), z->services.end(), v->service_names_[i]) != z->services.end();
    if (on != was) Client::get().service(z->name, v->service_names_[i], on, v->permanent(), done_cb);
  }, this);
  g1->resizable(services_);
  g1->end();
  // ports
  Fl_Group *g2 = new Fl_Group(dx, ty, dw, th, "Ports");
  ports_ = new Fl_Hold_Browser(dx + 6, ty + 6, dw - 130, th - 12);
  ports_->textsize(13);
  Fl_Button *padd = new Fl_Button(dx + dw - 118, ty + 6, 112, 28, "Add…");
  padd->callback([](Fl_Widget *, void *d) {
    FirewallView *v = (FirewallView *)d;
    if (!v->zone()) return;
    const char *t = fl_input("Port to open (for example 8080/tcp, 27000-27100/udp):", "");
    if (!t || !*t) return;
    std::string port, proto;
    if (!parse_port(t, port, proto)) {
      fl_alert("\"%s\" is not a port. Give a number or a range and tcp or udp, like 8080/tcp.", t);
      return;
    }
    Client::get().port(v->zone_, port, proto, true, v->permanent(), done_cb);
  }, this);
  Fl_Button *prm = new Fl_Button(dx + dw - 118, ty + 40, 112, 28, "Remove");
  prm->callback([](Fl_Widget *, void *d) {
    FirewallView *v = (FirewallView *)d;
    const Zone *z = v->zone();
    int i = v->ports_->value() - 1;
    if (!z || i < 0 || i >= (int)z->ports.size()) return;
    Client::get().port(z->name, z->ports[i].first, z->ports[i].second, false, v->permanent(), done_cb);
  }, this);
  g2->resizable(ports_);
  g2->end();
  // interfaces and sources
  Fl_Group *g3 = new Fl_Group(dx, ty, dw, th, "Interfaces && Sources");
  int half = (th - 12) / 2;
  Fl_Box *il = new Fl_Box(dx + 6, ty + 6, 300, 22, "Network interfaces in this zone");
  il->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  ifaces_ = new Fl_Hold_Browser(dx + 6, ty + 30, dw - 250, half - 30);
  ifaces_->textsize(13);
  iface_zone_ = new Fl_Choice(dx + dw - 238, ty + 30, 232, 28);
  iface_zone_->tooltip("Move an interface of the computer to this zone");
  iface_zone_->callback([](Fl_Widget *w, void *d) {
    FirewallView *v = (FirewallView *)d;
    int i = ((Fl_Choice *)w)->value();
    const State &st = Client::get().state();
    if (v->filling_ || i <= 0 || i > (int)st.interfaces.size() || !v->zone()) return;
    Client::get().interface_zone(st.interfaces[i - 1], v->zone_, v->permanent(), done_cb);
  }, this);
  Fl_Button *irm = new Fl_Button(dx + dw - 238, ty + 64, 232, 28, "Remove from Zone");
  irm->callback([](Fl_Widget *, void *d) {
    FirewallView *v = (FirewallView *)d;
    const Zone *z = v->zone();
    int i = v->ifaces_->value() - 1;
    if (!z || i < 0 || i >= (int)z->interfaces.size()) return;
    Client::get().interface_zone(z->interfaces[i], "", v->permanent(), done_cb);
  }, this);
  Fl_Box *sl = new Fl_Box(dx + 6, ty + half + 6, 400, 22, "Source addresses (IP or network, e.g. 192.168.1.0/24)");
  sl->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  sources_ = new Fl_Hold_Browser(dx + 6, ty + half + 30, dw - 250, th - half - 36);
  sources_->textsize(13);
  Fl_Button *sadd = new Fl_Button(dx + dw - 238, ty + half + 30, 232, 28, "Add Source…");
  sadd->callback([](Fl_Widget *, void *d) {
    FirewallView *v = (FirewallView *)d;
    if (!v->zone()) return;
    const char *t = fl_input("Address or network whose traffic belongs to this zone:", "192.168.1.0/24");
    if (!t || !*fld::trim(t).c_str()) return;
    Client::get().source(v->zone_, fld::trim(t), true, v->permanent(), done_cb);
  }, this);
  Fl_Button *srm = new Fl_Button(dx + dw - 238, ty + half + 64, 232, 28, "Remove Source");
  srm->callback([](Fl_Widget *, void *d) {
    FirewallView *v = (FirewallView *)d;
    const Zone *z = v->zone();
    int i = v->sources_->value() - 1;
    if (!z || i < 0 || i >= (int)z->sources.size()) return;
    Client::get().source(z->name, z->sources[i], false, v->permanent(), done_cb);
  }, this);
  g3->end();
  // masquerading and port forwarding
  Fl_Group *g4 = new Fl_Group(dx, ty, dw, th, "Forwarding");
  masq_ = new Fl_Check_Button(dx + 6, ty + 6, dw - 12, 26, "Masquerading (share this computer's connection, NAT)");
  masq_->callback([](Fl_Widget *w, void *d) {
    FirewallView *v = (FirewallView *)d;
    if (v->filling_ || !v->zone()) return;
    Client::get().masquerade(v->zone_, ((Fl_Check_Button *)w)->value(), v->permanent(), done_cb);
  }, this);
  Fl_Box *fl = new Fl_Box(dx + 6, ty + 40, 300, 22, "Port forwarding");
  fl->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  forwards_ = new Fl_Hold_Browser(dx + 6, ty + 64, dw - 130, th - 70);
  forwards_->textsize(13);
  forwards_->format_char(0);
  Fl_Button *fadd = new Fl_Button(dx + dw - 118, ty + 64, 112, 28, "Add…");
  fadd->callback([](Fl_Widget *, void *d) {
    FirewallView *v = (FirewallView *)d;
    if (!v->zone()) return;
    const char *t = fl_input("Incoming port (e.g. 8080/tcp):", "");
    if (!t || !*t) return;
    ForwardPort f;
    if (!parse_port(t, f.port, f.protocol)) {
      fl_alert("\"%s\" is not a port.", t);
      return;
    }
    const char *to = fl_input("Forward to (port, or address:port):", "80");
    if (!to || !*to) return;
    std::string dest = fld::trim(to);
    size_t c = dest.rfind(':');
    if (c == std::string::npos && dest.find('.') == std::string::npos) f.to_port = dest;
    else if (c == std::string::npos) f.to_addr = dest;
    else {
      f.to_addr = dest.substr(0, c);
      f.to_port = dest.substr(c + 1);
    }
    Client::get().forward_port(v->zone_, f, true, v->permanent(), done_cb);
  }, this);
  Fl_Button *frm = new Fl_Button(dx + dw - 118, ty + 98, 112, 28, "Remove");
  frm->callback([](Fl_Widget *, void *d) {
    FirewallView *v = (FirewallView *)d;
    const Zone *z = v->zone();
    int i = v->forwards_->value() - 1;
    if (!z || i < 0 || i >= (int)z->forward_ports.size()) return;
    Client::get().forward_port(z->name, z->forward_ports[i], false, v->permanent(), done_cb);
  }, this);
  g4->resizable(forwards_);
  g4->end();
  // right-click menus: the same as the buttons next to each list
  fld::set_context_menu(ports_, [padd, prm](fld::Menu &m) { m.button(padd).button(prm); });
  fld::set_context_menu(ifaces_, [irm](fld::Menu &m) { m.button(irm); });
  fld::set_context_menu(sources_, [sadd, srm](fld::Menu &m) { m.button(sadd).button(srm); });
  fld::set_context_menu(forwards_, [fadd, frm](fld::Menu &m) { m.button(fadd).button(frm); });
  fld::set_context_menu(zones_, [this](fld::Menu &m) {
    m.button(reload_).button(to_perm_).divider().button(permanent_).button(panic_);
  });
  tabs_->end();
  tabs_->resizable(g1);
  empty_ = new Fl_Box(X, Y + 34, W, H - 34);
  empty_->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER | FL_ALIGN_WRAP);
  empty_->labelcolor(fld::palette().fg_dim);
  empty_->hide();
  end();
  resizable(tabs_);
  Client::get().start();
  refresh();
}

const Zone *FirewallView::zone() const { return Client::get().state().zone(zone_); }

bool FirewallView::permanent() const { return permanent_->value(); }

void FirewallView::refresh() {
  const State &st = Client::get().state();
  bool on = st.running;
  for (Fl_Widget *w : {(Fl_Widget *)default_zone_, (Fl_Widget *)panic_, (Fl_Widget *)permanent_,
                       (Fl_Widget *)zones_, (Fl_Widget *)tabs_, (Fl_Widget *)zone_info_, (Fl_Widget *)reload_,
                       (Fl_Widget *)to_perm_})
    on ? w->show() : w->hide();
  on ? empty_->hide() : empty_->show();
  if (!on) {
    status_->label("The firewall is not running");
    empty_->label("firewalld is not running.\n\nInstall firewalld and start firewalld.service to use the firewall.");
    redraw();
    return;
  }
  filling_ = true;
  status_s_ = std::string(st.panic ? "Panic mode: all traffic is blocked" : "The firewall is on") +
              (st.version.empty() ? "" : "  (firewalld " + st.version + ")");
  status_->copy_label(status_s_.c_str());
  status_->labelcolor(st.panic ? fld::palette().danger : fld::palette().fg);
  panic_->value(st.panic);
  zone_names_.clear();
  default_zone_->clear();
  int keep = 0;
  zones_->clear();
  for (auto &z : st.zones) {
    zone_names_.push_back(z.name);
    default_zone_->add(z.name.c_str());
    if (z.name == st.default_zone) default_zone_->value((int)zone_names_.size() - 1);
    std::string label = z.name + (z.name == st.default_zone ? "  (default)" : "") + (z.active ? "  ●" : "");
    zones_->add(label.c_str());
    if (z.name == zone_) keep = zones_->size();
  }
  if (!keep) {
    zone_ = st.default_zone;
    for (size_t i = 0; i < zone_names_.size(); i++)
      if (zone_names_[i] == zone_) keep = (int)i + 1;
  }
  zones_->value(keep);
  filling_ = false;
  fill_zone();
}

void FirewallView::fill_zone() {
  const State &st = Client::get().state();
  const Zone *z = zone();
  filling_ = true;
  if (!z) {
    zone_info_->label("");
    filling_ = false;
    return;
  }
  info_s_ = (z->short_name.empty() ? z->name : z->short_name) + ": " + z->description;
  info_s_ = fld::escape_at(info_s_ + "\n" + target_text(z->target) + ".");
  zone_info_->label(info_s_.c_str());
  // services: the zone's first, then the rest by name
  int top = services_->vposition();
  services_->clear();
  service_names_.clear();
  std::vector<const Service *> list;
  for (auto &s : st.services) list.push_back(&s);
  std::stable_sort(list.begin(), list.end(), [z](const Service *a, const Service *b) {
    bool ia = std::find(z->services.begin(), z->services.end(), a->name) != z->services.end();
    bool ib = std::find(z->services.begin(), z->services.end(), b->name) != z->services.end();
    return ia > ib;
  });
  for (auto *s : list) {
    bool in = std::find(z->services.begin(), z->services.end(), s->name) != z->services.end();
    std::string ports;
    for (auto &p : s->ports) ports += (ports.empty() ? "" : ", ") + p.first + "/" + p.second;
    std::string label = s->name + (s->short_name.empty() || s->short_name == s->name ? "" : " — " + s->short_name) +
                        (ports.empty() ? "" : "  [" + ports + "]");
    services_->add(fld::escape_at(label).c_str(), in);
    service_names_.push_back(s->name);
  }
  // services the zone has that firewalld does not list (removed definitions)
  for (auto &n : z->services)
    if (!st.service(n)) {
      services_->add(n.c_str(), 1);
      service_names_.push_back(n);
    }
  services_->vposition(top);
  ports_->clear();
  for (auto &p : z->ports) ports_->add((p.first + "/" + p.second).c_str());
  ifaces_->clear();
  for (auto &i : z->interfaces) ifaces_->add(i.c_str());
  sources_->clear();
  for (auto &s : z->sources) sources_->add(s.c_str());
  iface_zone_->clear();
  iface_zone_->add("Add an interface…");
  for (auto &i : st.interfaces) {
    std::string in;
    for (auto &oz : st.zones)
      if (std::find(oz.interfaces.begin(), oz.interfaces.end(), i) != oz.interfaces.end()) in = oz.name;
    iface_zone_->add((i + (in.empty() ? "" : "  (" + in + ")")).c_str());
  }
  iface_zone_->value(0);
  masq_->value(z->masquerade);
  forwards_->clear();
  for (auto &f : z->forward_ports) forwards_->add(f.text().c_str());
  filling_ = false;
  redraw();
}

} // namespace fw
