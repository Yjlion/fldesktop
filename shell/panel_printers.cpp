// fl-panel: the printer button, shown while the user's jobs are printing.
//
// A worker thread asks CUPS for the user's active jobs (every 1.5 s while
// there are some, else every 3 s). When a job leaves the queue the panel
// sends a notification saying whether it was printed.
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/fl_draw.H>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "cupsclient.h"
#include "fld/icons.h"
#include "fld/notify.h"
#include "fld/util.h"
#include "panel.h"

namespace {

fld::FlatButton *g_btn = nullptr;
std::vector<prn::Job> g_jobs;            // FLTK thread copy
std::map<int, prn::Job> g_seen;          // active jobs seen before (FLTK thread)
std::vector<prn::Printer> g_printers;
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_kick = false;
std::string g_tip;

struct Snapshot {
  std::vector<prn::Job> active, finished;
  std::vector<prn::Printer> printers;
};

void update_button() {
  bool vis = !g_jobs.empty();
  bool problem = false;
  for (auto &j : g_jobs)
    for (auto &p : g_printers)
      if (p.name == j.printer) {
        if (p.state == 5) problem = true;
        for (auto &r : p.reasons)
          if (fld::ends_with(r, "-error")) problem = true;
      }
  Fl_Image *img = fld::symbolic_icon(problem ? "printer-warning" : "printer-printing", 18, panel_fg());
  if (!img) img = fld::symbolic_icon("printer", 18, panel_fg());
  g_btn->icon_image(img);
  if (!g_jobs.empty()) {
    const prn::Job &j = g_jobs.back(); // the oldest active job
    g_tip = (g_jobs.size() == 1 ? "Printing " : std::to_string(g_jobs.size()) + " documents; printing ") + "\"" +
            (j.title.empty() ? "untitled" : j.title) + "\" on " + j.printer;
    if (problem) g_tip += "\nThe printer needs attention";
    g_btn->tooltip(g_tip.c_str());
  }
  if (vis != (bool)g_btn->visible()) {
    vis ? g_btn->show() : g_btn->hide();
    relayout();
  }
  g_btn->redraw();
}

void apply(Snapshot *s) {
  std::unique_ptr<Snapshot> snap(s);
  g_printers = snap->printers;
  std::set<int> now;
  for (auto &j : snap->active) now.insert(j.id);
  // jobs that left the queue: say how they ended
  for (auto it = g_seen.begin(); it != g_seen.end();) {
    if (now.count(it->first)) {
      ++it;
      continue;
    }
    const prn::Job *done = nullptr;
    for (auto &f : snap->finished)
      if (f.id == it->first) done = &f;
    fld::Notification n;
    n.app_name = "Printers";
    n.desktop_entry = "fl-printers";
    n.category = "transfer";
    std::string title = it->second.title.empty() ? "Untitled document" : it->second.title;
    if (!done || done->state == 9) {
      n.summary = "Printed";
      n.body = "\"" + title + "\" was printed on " + it->second.printer + ".";
      n.icon = "printer";
      n.transient = true;
    } else if (done->state == 7) {
      n.summary = "Printing canceled";
      n.body = "\"" + title + "\" was canceled.";
      n.icon = "printer";
      n.transient = true;
    } else {
      n.summary = "Printing failed";
      n.body = "\"" + title + "\" could not be printed on " + it->second.printer + ".";
      n.icon = "printer-error";
      n.urgency = 2;
      n.actions = {{"default", "Show"}};
    }
    fld::notify(n, [](const std::string &) { fld::spawn({"fl-printers"}); });
    it = g_seen.erase(it);
  }
  for (auto &j : snap->active) g_seen[j.id] = j;
  g_jobs = snap->active;
  update_button();
}

void poll_thread() {
  std::set<int> last_active;
  bool first = true;
  while (true) {
    Snapshot *s = new Snapshot;
    s->active = prn::jobs(true, "", true);
    std::set<int> ids;
    for (auto &j : s->active) ids.insert(j.id);
    bool left = false;
    for (int id : last_active)
      if (!ids.count(id)) left = true;
    if (left) s->finished = prn::jobs(false, "", true);
    if (!s->active.empty() || left) s->printers = prn::printers();
    // nothing printing now or before: no need to wake the panel up
    bool idle = ids.empty() && last_active.empty() && !first;
    first = false;
    last_active = ids;
    if (idle) delete s;
    else Fl::awake([](void *d) { apply((Snapshot *)d); }, s);
    std::unique_lock<std::mutex> l(g_mutex);
    g_cv.wait_for(l, std::chrono::milliseconds(ids.empty() ? 3000 : 1500), [] { return g_kick; });
    g_kick = false;
  }
}

void kick() {
  std::lock_guard<std::mutex> l(g_mutex);
  g_kick = true;
  g_cv.notify_one();
}

void printers_cb(Fl_Widget *w, void *) {
  if (panel_popup_toggle(w)) return; // open: a second click closes it
  const int W = 380;
  int n = std::max<int>(1, g_jobs.size());
  int H = 44 + n * 50 + 46;
  PanelPopup pop(W, std::min(H, 420));
  const fld::Palette &p = fld::palette();
  Fl_Box *title = new Fl_Box(10, 8, W - 20, 28, "Printing");
  title->labelfont(FL_HELVETICA_BOLD);
  title->labelsize(14);
  title->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
  int y = 44;
  static std::vector<std::string> labels;
  labels.clear();
  labels.reserve(g_jobs.size() * 2);
  for (auto it = g_jobs.rbegin(); it != g_jobs.rend() && y < 420 - 96; ++it) {
    const prn::Job &j = *it;
    labels.push_back(fld::escape_at(j.title.empty() ? "Untitled document" : j.title));
    Fl_Box *t = new Fl_Box(12, y, W - 120, 22, labels.back().c_str());
    t->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    labels.push_back(fld::escape_at(prn::job_state_text(j.state) + " on " + j.printer));
    Fl_Box *s = new Fl_Box(12, y + 22, W - 120, 20, labels.back().c_str());
    s->labelsize(12);
    s->labelcolor(p.fg_dim);
    s->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    Fl_Button *c = new Fl_Button(W - 100, y + 8, 88, 28, "Cancel");
    c->labelsize(12);
    c->callback([](Fl_Widget *b, void *d) {
      prn::cancel_job((int)(long)d);
      kick();
      b->deactivate();
    }, (void *)(long)j.id);
    y += 50;
  }
  if (g_jobs.empty()) {
    Fl_Box *none = new Fl_Box(12, y, W - 24, 40, "Nothing is printing.");
    none->labelcolor(p.fg_dim);
    y += 50;
  }
  Fl_Button *settings = new Fl_Button(W - 152, pop.h() - 38, 140, 28, "Printer Settings");
  settings->labelsize(12);
  settings->callback([](Fl_Widget *b, void *) {
    fld::spawn({"fl-printers"});
    b->window()->hide();
  });
  pop.end();
  pop.popup_at(w, false);
}

} // namespace

fld::FlatButton *printer_indicator_create() {
  g_btn = make_btn(34, nullptr, "Printing", printers_cb);
  panel_context_menu(g_btn, [](fld::Menu &m) {
    m.item("Printers…", [] { fld::spawn({"fl-printers"}); });
    m.item("Printer Settings…", [] { fld::spawn({"fl-settings", "printers"}); }).divider();
    panel_menu_items(m);
  });
  g_btn->hide();
  std::thread(poll_thread).detach();
  return g_btn;
}
