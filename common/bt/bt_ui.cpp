#include "bt_ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>

#include <algorithm>

#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

namespace bt {

std::string device_status(const Device &d) {
  std::string s;
  if (d.busy) s = "Working…";
  else if (d.connected) s = "Connected";
  else if (d.blocked) s = "Blocked";
  else if (d.paired) s = "Not connected";
  else s = d.has_rssi ? "Nearby" : "Not paired";
  if (d.battery >= 0) s += " · battery " + std::to_string(d.battery) + "%";
  return s;
}

void show_error(const std::string &what, const std::string &err) {
  if (!err.empty()) fl_alert("%s\n\n%s", what.c_str(), err.c_str());
}

// ---- agent dialogs ---------------------------------------------------------------

namespace {

struct AgentDialog {
  Fl_Double_Window *win;
  Fl_Input *input = nullptr;
  std::function<void(bool, const std::string &)> reply;
  bool answered = false;
  void answer(bool ok) {
    if (answered) return;
    answered = true;
    std::string v = input ? fld::trim(input->value()) : "";
    win->hide();
    if (reply) reply(ok, v);
    Fl::delete_widget(win);
    delete this;
  }
};

} // namespace

void ask_agent(const AgentRequest &r, std::function<void(bool, const std::string &)> reply) {
  std::string name = r.device_name.empty() ? "The device" : r.device_name;
  std::string text, ok_label = "Pair";
  bool need_input = false, info_only = false;
  switch (r.kind) {
  case AgentRequest::PIN:
    text = "Enter the PIN for \"" + name + "\" (often 0000 or 1234, or the code shown on the device):";
    need_input = true;
    break;
  case AgentRequest::PASSKEY:
    text = "Enter the 6-digit passkey shown on \"" + name + "\":";
    need_input = true;
    break;
  case AgentRequest::DISPLAY_PIN:
    text = "Type the PIN " + r.value + " on \"" + name + "\" and press Enter there.";
    info_only = true;
    break;
  case AgentRequest::DISPLAY_PASSKEY:
    text = "Type the passkey " + r.value + " on \"" + name + "\" and press Enter there.";
    info_only = true;
    break;
  case AgentRequest::CONFIRM:
    text = "Does \"" + name + "\" show the passkey " + r.value + "?";
    ok_label = "It Matches";
    break;
  case AgentRequest::AUTHORIZE:
    text = "Allow \"" + name + "\" to pair with this computer?";
    ok_label = "Allow";
    break;
  case AgentRequest::AUTHORIZE_SERVICE:
    text = "Allow \"" + name + "\" to use a service of this computer?\n(" + r.value + ")";
    ok_label = "Allow";
    break;
  }
  AgentDialog *d = new AgentDialog;
  d->reply = std::move(reply);
  d->win = new Fl_Double_Window(420, need_input ? 190 : 160, "Bluetooth Pairing");
  // own app id: fl-panel's windows would otherwise be taken for the panel
  d->win->xclass("fl-bluetooth");
  Fl_Box *ic = new Fl_Box(16, 16, 48, 48);
  ic->image(fld::icon_first({"fl-bluetooth", "bluetooth"}, 48));
  Fl_Box *t = new Fl_Box(76, 14, 328, 64);
  t->copy_label(fld::escape_at(text).c_str());
  t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  if (r.kind == AgentRequest::CONFIRM || info_only) {
    t->resize(76, 14, 328, 40);
    Fl_Box *code = new Fl_Box(76, 56, 328, 40);
    code->copy_label(r.value.c_str());
    code->labelfont(FL_COURIER_BOLD);
    code->labelsize(26);
    code->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  }
  int by = d->win->h() - 46;
  if (need_input) {
    d->input = new Fl_Input(76, 92, 200, 30);
    if (r.kind == AgentRequest::PASSKEY) d->input->type(FL_INT_INPUT);
    d->input->maximum_size(r.kind == AgentRequest::PASSKEY ? 6 : 16);
  }
  if (info_only) {
    Fl_Return_Button *ok = new Fl_Return_Button(304, by, 100, 30, "Close");
    ok->callback([](Fl_Widget *, void *p) { ((AgentDialog *)p)->answer(true); }, d);
  } else {
    Fl_Button *no = new Fl_Button(176, by, 100, 30, "Cancel");
    no->callback([](Fl_Widget *, void *p) { ((AgentDialog *)p)->answer(false); }, d);
    Fl_Return_Button *ok = new Fl_Return_Button(284, by, 120, 30);
    ok->copy_label(ok_label.c_str());
    ok->callback([](Fl_Widget *, void *p) { ((AgentDialog *)p)->answer(true); }, d);
  }
  d->win->end();
  d->win->callback([](Fl_Widget *, void *p) { ((AgentDialog *)p)->answer(false); }, d);
  fld::set_window_icon(d->win, "fl-bluetooth");
  d->win->show();
  if (d->input) d->input->take_focus();
}

// ---- BluetoothView -------------------------------------------------------------------

BluetoothView::BluetoothView(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
  power_ = new Fl_Check_Button(X, Y, 200, 30, "Bluetooth");
  power_->labelfont(FL_HELVETICA_BOLD);
  power_->labelsize(FL_NORMAL_SIZE + 2);
  power_->callback([](Fl_Widget *w, void *) {
    bool on = ((Fl_Check_Button *)w)->value();
    Client::get().set_powered(on, [](const std::string &e) { show_error("Bluetooth could not be switched.", e); });
  });
  visible_ = new Fl_Check_Button(X + W - 260, Y, 260, 30, "Visible to other devices");
  visible_->callback([](Fl_Widget *w, void *) {
    Client::get().set_discoverable(((Fl_Check_Button *)w)->value(),
                                   [](const std::string &e) { show_error("The setting could not be changed.", e); });
  });
  summary_ = new Fl_Box(X, Y + 30, W, 24);
  summary_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  summary_->labelcolor(fld::palette().fg_dim);
  int lh = (H - 60 - 24 - 24 - 44) / 2;
  mine_label_ = new Fl_Box(X, Y + 60, W, 22, "My devices");
  mine_label_->labelfont(FL_HELVETICA_BOLD);
  mine_label_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  mine_ = new Fl_Hold_Browser(X, Y + 84, W, lh);
  nearby_label_ = new Fl_Box(X, Y + 84 + lh + 4, W, 22, "Nearby devices");
  nearby_label_->labelfont(FL_HELVETICA_BOLD);
  nearby_label_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  nearby_ = new Fl_Hold_Browser(X, Y + 84 + lh + 28, W, H - (84 + lh + 28) - 44);
  for (Fl_Hold_Browser *b : {mine_, nearby_}) {
    b->format_char(0);
    b->column_char('\t');
    b->textsize(14);
    b->callback(list_cb, this);
    b->when(FL_WHEN_CHANGED | FL_WHEN_NOT_CHANGED);
  }
  int by = Y + H - 34;
  connect_ = new Fl_Button(X, by, 120, 30, "Connect");
  connect_->callback([](Fl_Widget *, void *d) {
    BluetoothView *v = (BluetoothView *)d;
    const Device *dev = Client::get().state().device(v->selected_);
    if (!dev) return;
    std::string path = dev->path;
    for (auto &x : Client::get().state_.devices)
      if (x.path == path) x.busy = true;
    auto done = [v, path](const std::string &e) {
      for (auto &x : Client::get().state_.devices)
        if (x.path == path) x.busy = false;
      v->refresh();
      show_error("The device could not be connected.", e);
    };
    if (dev->connected) Client::get().disconnect(path, done);
    else Client::get().connect(path, done);
    v->refresh();
  }, this);
  pair_ = new Fl_Button(X + 126, by, 120, 30, "Pair");
  pair_->callback([](Fl_Widget *, void *d) {
    BluetoothView *v = (BluetoothView *)d;
    const Device *dev = Client::get().state().device(v->selected_);
    if (!dev) return;
    std::string path = dev->path;
    for (auto &x : Client::get().state_.devices)
      if (x.path == path) x.busy = true;
    Client::get().pair(path, [v, path](const std::string &e) {
      for (auto &x : Client::get().state_.devices)
        if (x.path == path) x.busy = false;
      v->refresh();
      show_error("The device could not be paired.", e);
    });
    v->refresh();
  }, this);
  forget_ = new Fl_Button(X + 252, by, 120, 30, "Forget");
  forget_->callback([](Fl_Widget *, void *d) {
    BluetoothView *v = (BluetoothView *)d;
    const Device *dev = Client::get().state().device(v->selected_);
    if (!dev) return;
    if (fl_choice("Forget \"%s\"?\nIt has to be paired again to be used.", "Cancel", "Forget", nullptr,
                  dev->display_name().c_str()) != 1)
      return;
    Client::get().remove(dev->path, [](const std::string &e) { show_error("The device could not be removed.", e); });
  }, this);
  trusted_ = new Fl_Check_Button(X + 384, by, 200, 30, "Trusted");
  trusted_->tooltip("A trusted device may connect without asking");
  trusted_->callback([](Fl_Widget *w, void *d) {
    BluetoothView *v = (BluetoothView *)d;
    Client::get().set_trusted(v->selected_, ((Fl_Check_Button *)w)->value(),
                              [](const std::string &e) { show_error("The setting could not be changed.", e); });
  }, this);
  empty_ = new Fl_Box(X, Y + 60, W, H - 60);
  empty_->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER | FL_ALIGN_WRAP);
  empty_->labelcolor(fld::palette().fg_dim);
  empty_->hide();
  end();
  resizable(nearby_);
  Client::get().start();
  refresh();
}

BluetoothView::~BluetoothView() { set_discovery(false); }

void BluetoothView::set_discovery(bool on) {
  if (on == discovering_) return;
  discovering_ = on;
  Client::get().discovery(on);
}

void BluetoothView::list_cb(Fl_Widget *w, void *d) {
  BluetoothView *v = (BluetoothView *)d;
  Fl_Hold_Browser *b = (Fl_Hold_Browser *)w;
  auto &paths = b == v->mine_ ? v->mine_paths_ : v->nearby_paths_;
  int i = b->value() - 1;
  if (i < 0 || i >= (int)paths.size()) return;
  (b == v->mine_ ? v->nearby_ : v->mine_)->deselect();
  v->selected_ = paths[i];
  v->update_buttons();
  // double click: connect / pair
  if (Fl::event_clicks() > 0 && Fl::event() == FL_RELEASE) {
    Fl::event_clicks(0);
    const Device *dev = Client::get().state().device(v->selected_);
    if (dev && !dev->busy) (dev->paired ? v->connect_ : v->pair_)->do_callback();
  }
}

void BluetoothView::select(const std::string &path) {
  selected_ = path;
  update_buttons();
}

void BluetoothView::update_buttons() {
  const Device *d = Client::get().state().device(selected_);
  const Adapter *a = Client::get().state().adapter();
  bool on = a && a->powered;
  if (!d || !on || d->busy) {
    connect_->deactivate();
    pair_->deactivate();
    forget_->deactivate();
    trusted_->deactivate();
  } else {
    d->paired ? connect_->activate() : connect_->deactivate();
    connect_->label(d->connected ? "Disconnect" : "Connect");
    d->paired ? pair_->deactivate() : pair_->activate();
    forget_->activate();
    d->paired ? trusted_->activate() : trusted_->deactivate();
    trusted_->value(d->trusted);
  }
  if (!d) connect_->label("Connect");
}

void BluetoothView::refresh() {
  const State &st = Client::get().state();
  const Adapter *a = st.adapter();
  bool show = st.running && a;
  for (Fl_Widget *w : {(Fl_Widget *)mine_, (Fl_Widget *)nearby_, (Fl_Widget *)mine_label_,
                       (Fl_Widget *)nearby_label_, (Fl_Widget *)connect_, (Fl_Widget *)pair_, (Fl_Widget *)forget_,
                       (Fl_Widget *)trusted_})
    show ? w->show() : w->hide();
  show ? empty_->hide() : empty_->show();
  if (!st.running) {
    empty_->label("The Bluetooth service is not running.\n\nInstall BlueZ (bluez) and start bluetooth.service.");
    power_->deactivate();
    visible_->deactivate();
    summary_->label("");
    set_discovery(false);
    redraw();
    return;
  }
  if (!a) {
    empty_->label("No Bluetooth adapter was found.");
    power_->deactivate();
    visible_->deactivate();
    summary_->label("");
    set_discovery(false);
    redraw();
    return;
  }
  power_->activate();
  power_->value(a->powered);
  a->powered ? visible_->activate() : visible_->deactivate();
  visible_->value(a->discoverable);
  if (!a->powered) summary_s_ = st.rfkill_hard ? "Turned off by a hardware switch" : "Bluetooth is off";
  else summary_s_ = "This computer is \"" + (a->alias.empty() ? a->name : a->alias) + "\"" +
                    (a->discovering ? " · searching for devices…" : "");
  summary_->copy_label(fld::escape_at(summary_s_).c_str());
  set_discovery(a->powered && visible_r());
  auto fill = [&](Fl_Hold_Browser *b, std::vector<std::string> &paths, bool paired) {
    int keep = 0;
    b->clear();
    paths.clear();
    for (auto &d : st.devices) {
      if (d.paired != paired) continue;
      // nameless devices (BlueZ shows their address) are noise in the nearby list
      if (!paired && d.name.empty()) continue;
      std::string line = d.display_name() + "\t" + device_status(d);
      b->add(line.c_str());
      b->icon(b->size(), fld::icon_first({device_icon(d.icon, d.device_class), "bluetooth"}, 24));
      paths.push_back(d.path);
      if (d.path == selected_) keep = b->size();
    }
    static int widths[] = {0, 0};
    widths[0] = std::max(120, b->w() - 220);
    b->column_widths(widths);
    if (keep) b->value(keep);
    if (paths.empty()) b->add(paired ? "\tNo paired devices" : (a->powered ? "\tSearching…" : "\tBluetooth is off"));
  };
  fill(mine_, mine_paths_, true);
  fill(nearby_, nearby_paths_, false);
  if (!st.device(selected_)) selected_.clear();
  update_buttons();
  redraw();
}

} // namespace bt
