#include "printers_ui.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

namespace prn {

namespace {

std::thread::id g_ui_thread = std::this_thread::get_id();

std::string menu_escape(const std::string &l) {
  std::string esc;
  for (char c : l) {
    if (c == '/' || c == '&' || c == '_' || c == '@' || c == '\\') esc += '\\';
    esc += c;
  }
  return esc;
}

bool ask_password_ui(const std::string &resource, std::string &user, std::string &pw) {
  Fl_Double_Window w(400, 210, "Authentication Required");
  Fl_Box *ic = new Fl_Box(16, 16, 40, 40);
  ic->image(fld::icon_first({"dialog-password", "system-lock-screen", "printer"}, 40));
  Fl_Box *m = new Fl_Box(68, 12, 316, 50,
                         "Changing printer settings needs the password of an administrator (a member of the "
                         "lpadmin group, or root).");
  m->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  Fl_Input *u = new Fl_Input(160, 72, 224, 28, "User name:");
  u->value(user.c_str());
  Fl_Secret_Input *p = new Fl_Secret_Input(160, 106, 224, 28, "Password:");
  Fl_Button *cancel = new Fl_Button(176, 164, 100, 30, "Cancel");
  Fl_Return_Button *ok = new Fl_Return_Button(284, 164, 100, 30, "OK");
  static int result;
  result = 0;
  cancel->callback([](Fl_Widget *b, void *) { result = 0; b->window()->hide(); });
  ok->callback([](Fl_Widget *b, void *) { result = 1; b->window()->hide(); });
  w.end();
  w.set_modal();
  w.show();
  p->take_focus();
  while (w.shown()) Fl::wait();
  if (!result) return false;
  user = u->value();
  pw = p->value();
  return true;
}

} // namespace

void install_password_prompt() {
  g_ui_thread = std::this_thread::get_id();
  set_password_prompt([](const std::string &res, std::string &user, std::string &pw) -> bool {
    if (std::this_thread::get_id() == g_ui_thread) return ask_password_ui(res, user, pw);
    // from a worker thread: ask in the FLTK thread and wait
    struct Ctx {
      std::mutex m;
      std::condition_variable cv;
      bool done = false, ok = false;
      std::string res, user, pw;
    } c;
    c.res = res;
    c.user = user;
    Fl::awake(
        [](void *d) {
          Ctx *c = (Ctx *)d;
          std::string u = c->user, p;
          bool ok = ask_password_ui(c->res, u, p);
          std::lock_guard<std::mutex> l(c->m);
          c->ok = ok;
          c->user = u;
          c->pw = p;
          c->done = true;
          c->cv.notify_one();
        },
        &c);
    std::unique_lock<std::mutex> l(c.m);
    c.cv.wait(l, [&] { return c.done; });
    user = c.user;
    pw = c.pw;
    return c.ok;
  });
}

bool show_error(const std::string &what, const std::string &err) {
  if (err.empty()) return false;
  fl_alert("%s\n\n%s", what.c_str(), err.c_str());
  return true;
}

// ---- Add Printer dialog -------------------------------------------------------

namespace {

struct AddDialog : std::enable_shared_from_this<AddDialog> {
  Fl_Double_Window *win;
  Fl_Hold_Browser *devices;
  Fl_Box *status;
  Fl_Input *uri, *name, *info, *location;
  Fl_Choice *driver;
  Fl_Button *rescan;
  Fl_Return_Button *add;
  std::vector<Device> found;
  std::vector<Driver> drivers;
  std::string device_id;
  bool driverless = false;
  std::atomic<int> generation{0};
  std::string result;
  bool busy = false;

  void set_status(const std::string &s) {
    status->copy_label(fld::escape_at(s).c_str());
    status->redraw();
  }

  void scan() {
    set_status("Searching for printers…");
    rescan->deactivate();
    int gen = ++generation;
    auto self = shared_from_this();
    std::thread([self, gen] {
      Error err;
      auto list = discover(5, &err);
      // the dialog may be closed meanwhile: generation guards against stale results
      struct R {
        std::shared_ptr<AddDialog> d;
        int gen;
        std::vector<Device> list;
        Error err;
      };
      Fl::awake(
          [](void *p) {
            std::unique_ptr<R> r((R *)p);
            if (r->gen != r->d->generation) return;
            r->d->scan_done(r->list, r->err);
          },
          new R{self, gen, list, err});
    }).detach();
  }

  void scan_done(const std::vector<Device> &list, const Error &err) {
    found = list;
    devices->clear();
    for (auto &d : found) {
      std::string label = d.info.empty() ? d.uri : d.info;
      if (!d.make_model.empty() && d.make_model != "Unknown" && d.make_model != label) label += " — " + d.make_model;
      devices->add(label.c_str());
      bool net = d.device_class == "network";
      devices->icon(devices->size(), fld::icon_first({net ? "printer-network" : "printer"}, 20));
    }
    rescan->activate();
    if (!err.empty()) set_status("Search failed: " + err);
    else if (found.empty()) set_status("No printers found. Enter the printer's address below.");
    else set_status(std::to_string(found.size()) + " printers found. Pick one or enter an address.");
  }

  void device_picked() {
    int i = devices->value() - 1;
    if (i < 0 || i >= (int)found.size()) return;
    const Device &d = found[i];
    uri->value(d.uri.c_str());
    std::string desc = !d.make_model.empty() && d.make_model != "Unknown" ? d.make_model : d.info;
    info->value(desc.c_str());
    name->value(queue_name(desc.empty() ? "Printer" : desc).c_str());
    location->value(d.location.c_str());
    device_id = d.device_id;
    load_drivers();
  }

  void load_drivers() {
    std::string u = fld::trim(uri->value());
    driverless = is_driverless_uri(u);
    driver->clear();
    drivers.clear();
    if (driverless) driver->add("Driverless (IPP Everywhere)");
    driver->add("Searching for drivers…");
    driver->value(0);
    driver->deactivate();
    std::string id = device_id, make = device_id_field(device_id, "MFG");
    if (make.empty()) {
      std::string desc = info->value();
      make = desc.substr(0, desc.find(' '));
    }
    int gen = ++generation;
    auto self = shared_from_this();
    std::thread([self, gen, id, make] {
      Error err;
      auto list = prn::drivers(id, "", &err);
      if (list.empty() && !make.empty()) list = prn::drivers("", make, &err);
      struct R {
        std::shared_ptr<AddDialog> d;
        int gen;
        std::vector<Driver> list;
      };
      Fl::awake(
          [](void *p) {
            std::unique_ptr<R> r((R *)p);
            if (r->gen != r->d->generation) return;
            r->d->drivers_done(r->list);
          },
          new R{self, gen, list});
    }).detach();
  }

  void drivers_done(const std::vector<Driver> &list) {
    drivers = list;
    driver->clear();
    if (driverless) driver->add("Driverless (IPP Everywhere)");
    for (auto &d : drivers) driver->add(menu_escape(d.make_model.empty() ? d.name : d.make_model).c_str());
    if (!driverless && drivers.empty()) driver->add("No driver found (try a driverless address)");
    driver->value(0);
    driver->activate();
    if (!driverless && drivers.empty()) driver->deactivate();
  }

  std::string chosen_ppd() const {
    int v = driver->value();
    if (driverless) {
      if (v == 0) return "everywhere";
      v--;
    }
    if (v >= 0 && v < (int)drivers.size()) return drivers[v].name;
    return "";
  }

  void do_add() {
    std::string u = fld::trim(uri->value()), n = fld::trim(name->value());
    if (u.empty() || u.find(':') == std::string::npos) {
      fl_alert("Enter the address of the printer, for example ipp://192.168.1.20/ipp/print or "
               "socket://192.168.1.20.");
      return;
    }
    if (n.empty() || queue_name(n) != n) {
      fl_alert("The name may only use letters, digits, '-' and '_' (it becomes the queue name).");
      return;
    }
    std::string ppd = chosen_ppd();
    if (ppd.empty()) {
      fl_alert("Choose a driver for this printer.");
      return;
    }
    busy = true;
    add->deactivate();
    set_status("Adding the printer…");
    std::string i = info->value(), l = location->value();
    auto self = shared_from_this();
    std::thread([self, n, u, ppd, i, l] {
      Error e = add_printer(n, u, ppd, i, l);
      struct R {
        std::shared_ptr<AddDialog> d;
        std::string name;
        Error err;
      };
      Fl::awake(
          [](void *p) {
            std::unique_ptr<R> r((R *)p);
            AddDialog *d = r->d.get();
            d->busy = false;
            d->add->activate();
            if (show_error("The printer could not be added.", r->err)) {
              d->set_status("");
              return;
            }
            d->result = r->name;
            d->win->hide();
          },
          new R{self, n, e});
    }).detach();
  }
};

} // namespace

std::string add_printer_dialog() {
  auto dp = std::make_shared<AddDialog>();
  AddDialog &d = *dp;
  Fl_Double_Window w(560, 520, "Add Printer");
  d.win = &w;
  Fl_Box *h = new Fl_Box(16, 10, 440, 26, "Printers found");
  h->labelfont(FL_HELVETICA_BOLD);
  h->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  d.rescan = new Fl_Button(456, 10, 88, 26, "Search");
  d.rescan->callback([](Fl_Widget *, void *p) { ((AddDialog *)p)->scan(); }, &d);
  d.devices = new Fl_Hold_Browser(16, 40, 528, 170);
  d.devices->format_char(0);
  d.devices->textsize(13);
  d.devices->callback([](Fl_Widget *, void *p) { ((AddDialog *)p)->device_picked(); }, &d);
  d.status = new Fl_Box(16, 212, 528, 24);
  d.status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  d.status->labelsize(12);
  d.status->labelcolor(fld::palette().fg_dim);
  d.uri = new Fl_Input(130, 244, 414, 28, "Address:");
  d.uri->tooltip("ipp://host/ipp/print, ipps://…, socket://host:9100, lpd://host/queue, usb://…");
  d.uri->when(FL_WHEN_RELEASE | FL_WHEN_ENTER_KEY);
  d.uri->callback([](Fl_Widget *, void *p) {
    AddDialog *a = (AddDialog *)p;
    a->device_id.clear();
    a->load_drivers();
  }, &d);
  d.name = new Fl_Input(130, 280, 414, 28, "Name:");
  d.info = new Fl_Input(130, 316, 414, 28, "Description:");
  d.location = new Fl_Input(130, 352, 414, 28, "Location:");
  d.driver = new Fl_Choice(130, 388, 414, 28, "Driver:");
  d.driver->add("Pick a printer or enter an address");
  d.driver->value(0);
  d.driver->deactivate();
  Fl_Box *note = new Fl_Box(130, 418, 414, 40,
                            "Most printers made since 2010 work driverless over the network (IPP Everywhere, "
                            "AirPrint) and need no driver.");
  note->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  note->labelsize(12);
  note->labelcolor(fld::palette().fg_dim);
  Fl_Button *cancel = new Fl_Button(328, 476, 104, 30, "Cancel");
  cancel->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  d.add = new Fl_Return_Button(440, 476, 104, 30, "Add");
  d.add->callback([](Fl_Widget *, void *p) { ((AddDialog *)p)->do_add(); }, &d);
  w.end();
  w.set_modal();
  fld::set_window_icon(&w, "printer");
  w.show();
  d.scan();
  while (w.shown() || d.busy) Fl::wait();
  d.generation++; // late results of the search threads are dropped
  return d.result;
}

// ---- PrintersView -------------------------------------------------------------

PrintersView::PrintersView(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
  list_ = new Fl_Hold_Browser(X, Y, 220, H - 40);
  list_->format_char(0);
  list_->textsize(14);
  list_->callback([](Fl_Widget *, void *d) {
    PrintersView *v = (PrintersView *)d;
    int i = v->list_->value() - 1;
    if (i >= 0 && i < (int)v->printers_.size()) {
      v->current_ = v->printers_[i].name;
      v->fill_detail();
      v->fill_jobs();
    }
  }, this);
  add_ = new Fl_Button(X, Y + H - 34, 108, 30, "Add…");
  add_->callback([](Fl_Widget *, void *d) {
    PrintersView *v = (PrintersView *)d;
    std::string n = add_printer_dialog();
    if (!n.empty()) {
      v->current_ = n;
      v->refresh();
    }
  }, this);
  remove_ = new Fl_Button(X + 112, Y + H - 34, 108, 30, "Remove");
  remove_->callback([](Fl_Widget *, void *d) {
    PrintersView *v = (PrintersView *)d;
    const Printer *p = v->printer();
    if (!p) return;
    if (fl_choice("Remove the printer \"%s\"?\nIts waiting jobs are canceled.", "Cancel", "Remove", nullptr,
                  p->name.c_str()) != 1)
      return;
    if (!show_error("The printer could not be removed.", delete_printer(p->name))) {
      v->current_.clear();
      v->refresh();
    }
  }, this);

  detail_ = new Fl_Group(X + 232, Y, W - 232, H);
  int dx = X + 232, dw = W - 232;
  icon_ = new Fl_Box(dx, Y, 48, 48);
  title_ = new Fl_Box(dx + 58, Y, dw - 58, 26);
  title_->labelfont(FL_HELVETICA_BOLD);
  title_->labelsize(FL_NORMAL_SIZE + 3);
  title_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  state_ = new Fl_Box(dx + 58, Y + 26, dw - 58, 22);
  state_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  state_->labelcolor(fld::palette().fg_dim);
  int y = Y + 60;
  info_ = new Fl_Input(dx + 110, y, dw - 110, 28, "Description:");
  info_->when(FL_WHEN_RELEASE | FL_WHEN_ENTER_KEY);
  location_ = new Fl_Input(dx + 110, y + 34, dw - 110, 28, "Location:");
  location_->when(FL_WHEN_RELEASE | FL_WHEN_ENTER_KEY);
  auto save_info = [](Fl_Widget *, void *d) {
    PrintersView *v = (PrintersView *)d;
    const Printer *p = v->printer();
    if (!p || (p->info == v->info_->value() && p->location == v->location_->value())) return;
    show_error("The printer could not be changed.", set_info(p->name, v->info_->value(), v->location_->value()));
    v->refresh();
  };
  info_->callback(save_info, this);
  location_->callback(save_info, this);
  model_ = new Fl_Box(dx + 110, y + 68, dw - 110, 22);
  model_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  Fl_Box *ml = new Fl_Box(dx, y + 68, 106, 22, "Model:");
  ml->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  conn_ = new Fl_Box(dx + 110, y + 92, dw - 110, 22);
  conn_->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  Fl_Box *cl = new Fl_Box(dx, y + 92, 106, 22, "Connection:");
  cl->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  y += 122;
  enabled_ = new Fl_Check_Button(dx + 110, y, 160, 26, "Enabled");
  enabled_->tooltip("A paused printer keeps jobs waiting");
  enabled_->callback([](Fl_Widget *w, void *d) {
    PrintersView *v = (PrintersView *)d;
    if (const Printer *p = v->printer())
      show_error("The printer could not be changed.", set_enabled(p->name, ((Fl_Check_Button *)w)->value()));
    v->refresh();
  }, this);
  accepting_ = new Fl_Check_Button(dx + 270, y, 170, 26, "Accepting jobs");
  accepting_->callback([](Fl_Widget *w, void *d) {
    PrintersView *v = (PrintersView *)d;
    if (const Printer *p = v->printer())
      show_error("The printer could not be changed.", set_accepting(p->name, ((Fl_Check_Button *)w)->value()));
    v->refresh();
  }, this);
  shared_ = new Fl_Check_Button(dx + 110, y + 28, 260, 26, "Shared with other computers");
  shared_->callback([](Fl_Widget *w, void *d) {
    PrintersView *v = (PrintersView *)d;
    if (const Printer *p = v->printer())
      show_error("The printer could not be changed.", set_shared(p->name, ((Fl_Check_Button *)w)->value()));
    v->refresh();
  }, this);
  y += 62;
  const char *labels[] = {"Paper size:", "Two-sided:", "Color:"};
  for (int i = 0; i < 3; i++) {
    opt_[i] = new Fl_Choice(dx + 110, y + i * 34, 220, 28, labels[i]);
    opt_[i]->callback([](Fl_Widget *w, void *d) {
      PrintersView *v = (PrintersView *)d;
      const Printer *p = v->printer();
      if (!p) return;
      for (int k = 0; k < 3; k++) {
        if (v->opt_[k] != w) continue;
        for (auto &o : v->options_) {
          const char *want = k == 0 ? "media" : k == 1 ? "sides" : "print-color-mode";
          if (o.key != want) continue;
          int c = ((Fl_Choice *)w)->value();
          if (c >= 0 && c < (int)o.choices.size())
            show_error("The setting could not be changed.", set_option(p->name, o.key, o.choices[c].first));
        }
      }
    }, this);
  }
  y += 3 * 34 + 6;
  default_ = new Fl_Button(dx + 110, y, 140, 30, "Set as Default");
  default_->callback([](Fl_Widget *, void *d) {
    PrintersView *v = (PrintersView *)d;
    if (const Printer *p = v->printer()) show_error("The default printer could not be set.", set_default(p->name));
    v->refresh();
  }, this);
  test_ = new Fl_Button(dx + 256, y, 140, 30, "Print Test Page");
  test_->callback([](Fl_Widget *, void *d) {
    PrintersView *v = (PrintersView *)d;
    const Printer *p = v->printer();
    if (!p) return;
    Error e;
    if (!print_test_page(p->name, &e)) show_error("The test page could not be printed.", e);
    v->refresh();
  }, this);
  y += 40;
  jobs_list_ = new Fl_Browser(dx, y + 24, dw, Y + H - (y + 24) - 40);
  jobs_list_->type(FL_HOLD_BROWSER);
  jobs_list_->format_char(0);
  jobs_list_->column_char('\t');
  jobs_list_->textsize(13);
  Fl_Box *jl = new Fl_Box(dx, y, 100, 22, "Jobs");
  jl->labelfont(FL_HELVETICA_BOLD);
  jl->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  show_done_ = new Fl_Check_Button(dx + dw - 160, y, 160, 22, "Show finished");
  show_done_->callback([](Fl_Widget *, void *d) { ((PrintersView *)d)->fill_jobs(); }, this);
  cancel_job_ = new Fl_Button(dx + dw - 120, Y + H - 34, 120, 30, "Cancel Job");
  cancel_job_->callback([](Fl_Widget *, void *d) {
    PrintersView *v = (PrintersView *)d;
    int i = v->jobs_list_->value() - 2; // first line is the header
    if (i < 0 || i >= (int)v->jobs_.size()) return;
    show_error("The job could not be canceled.", cancel_job(v->jobs_[i].id));
    v->fill_jobs();
  }, this);
  detail_->end();
  detail_->resizable(jobs_list_);
  empty_ = new Fl_Box(X + 232, Y, W - 232, H);
  empty_->labelcolor(fld::palette().fg_dim);
  empty_->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER | FL_ALIGN_WRAP);
  end();
  resizable(detail_);
  install_password_prompt();
  refresh();
  Fl::add_timeout(3.0, timer, this);
}

PrintersView::~PrintersView() { Fl::remove_timeout(timer, this); }

void PrintersView::timer(void *d) {
  PrintersView *v = (PrintersView *)d;
  if (v->visible_r()) {
    // only the state and jobs; keep edits in progress
    Error err;
    auto list = printers(&err);
    bool changed = list.size() != v->printers_.size();
    for (size_t i = 0; !changed && i < list.size(); i++)
      changed = list[i].name != v->printers_[i].name || list[i].state != v->printers_[i].state ||
                list[i].accepting != v->printers_[i].accepting || list[i].reasons != v->printers_[i].reasons ||
                list[i].is_default != v->printers_[i].is_default || list[i].active_jobs != v->printers_[i].active_jobs;
    if (changed) {
      v->printers_ = list;
      v->fill_list();
      if (Fl::focus() != v->info_ && Fl::focus() != v->location_) v->fill_detail();
    }
    v->fill_jobs();
  }
  Fl::repeat_timeout(3.0, timer, d);
}

const Printer *PrintersView::printer() const {
  for (auto &p : printers_)
    if (p.name == current_) return &p;
  return nullptr;
}

void PrintersView::select(const std::string &name) {
  current_ = name;
  fill_list();
  fill_detail();
  fill_jobs();
}

void PrintersView::refresh() {
  Error err;
  printers_ = printers(&err);
  if (!printer() && !printers_.empty()) {
    current_ = printers_[0].name;
    for (auto &p : printers_)
      if (p.is_default) current_ = p.name;
  }
  fill_list();
  fill_detail();
  fill_jobs();
  if (!err.empty()) {
    empty_->copy_label((err + "\n\nInstall and start CUPS (cups package, cups.service) to print.").c_str());
    add_->deactivate();
  } else {
    empty_->label("No printers are set up.\nClick \"Add…\" to add one.");
    add_->activate();
  }
}

void PrintersView::fill_list() {
  list_->clear();
  int sel = 0;
  for (auto &p : printers_) {
    std::string label = (p.info.empty() ? p.name : p.info) + (p.is_default ? "  (default)" : "");
    list_->add(label.c_str());
    const char *ic = p.state == 5 || !p.accepting ? "printer-error" : p.remote ? "printer-network" : "printer";
    list_->icon(list_->size(), fld::icon_first({ic, "printer"}, 32));
    if (p.name == current_) sel = list_->size();
  }
  list_->value(sel);
  printer() ? remove_->activate() : remove_->deactivate();
}

void PrintersView::fill_detail() {
  const Printer *p = printer();
  if (!p) {
    detail_->hide();
    empty_->show();
    return;
  }
  empty_->hide();
  detail_->show();
  icon_->image(fld::icon_first({p->remote ? "printer-network" : "printer"}, 48));
  title_s_ = fld::escape_at(p->info.empty() ? p->name : p->info);
  title_->label(title_s_.c_str());
  std::string st = printer_state_text(*p), msg = fld::trim(p->state_message);
  while (!msg.empty() && msg.back() == '.') msg.pop_back();
  if (!msg.empty() && fld::lower(msg) != fld::lower(st)) st += " — " + msg;
  state_s_ = fld::escape_at(st);
  state_->label(state_s_.c_str());
  if (Fl::focus() != info_) info_->value(p->info.c_str());
  if (Fl::focus() != location_) location_->value(p->location.c_str());
  model_s_ = fld::escape_at(p->make_model.empty() ? "Unknown" : p->make_model);
  model_->label(model_s_.c_str());
  conn_s_ = fld::escape_at(p->device_uri.empty() ? p->uri : p->device_uri);
  conn_->label(conn_s_.c_str());
  conn_->tooltip(conn_s_.c_str());
  enabled_->value(p->state != 5);
  accepting_->value(p->accepting);
  shared_->value(p->shared);
  p->is_default ? default_->deactivate() : default_->activate();
  options_ = options(p->name);
  const char *keys[] = {"media", "sides", "print-color-mode"};
  for (int i = 0; i < 3; i++) {
    opt_[i]->clear();
    const Option *o = nullptr;
    for (auto &op : options_)
      if (op.key == keys[i]) o = &op;
    if (!o) {
      opt_[i]->add("—");
      opt_[i]->value(0);
      opt_[i]->deactivate();
      continue;
    }
    int sel = 0;
    for (size_t k = 0; k < o->choices.size(); k++) {
      opt_[i]->add(menu_escape(o->choices[k].second).c_str());
      if (o->choices[k].first == o->value) sel = (int)k;
    }
    opt_[i]->value(sel);
    opt_[i]->activate();
  }
  redraw();
}

void PrintersView::fill_jobs() {
  if (!printer()) return;
  jobs_ = jobs(!show_done_->value(), current_, false);
  int keep = jobs_list_->value();
  jobs_list_->clear();
  static int widths[] = {60, 0, 110, 80, 0};
  widths[1] = std::max(120, jobs_list_->w() - 60 - 110 - 80 - 140);
  widths[3] = 80;
  jobs_list_->column_widths(widths);
  jobs_list_->add("Job\tDocument\tState\tSize\tSubmitted");
  for (auto &j : jobs_) {
    std::string line = std::to_string(j.id) + "\t" + (j.title.empty() ? "(untitled)" : j.title) + "\t" +
                       job_state_text(j.state) + "\t" + fld::human_size((unsigned long long)j.size_kb * 1024) +
                       "\t" + fld::format_time(j.created);
    jobs_list_->add(line.c_str());
  }
  if (keep > 1 && keep <= jobs_list_->size()) jobs_list_->value(keep);
  if (jobs_.empty()) jobs_list_->add("\tNo jobs");
}

void PrintersView::resize(int X, int Y, int W, int H) { Fl_Group::resize(X, Y, W, H); }

} // namespace prn
