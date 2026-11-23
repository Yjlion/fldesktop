// fl-services: the system's services (systemd or OpenRC). Start, stop and
// restart them and choose which start at boot; systemd's user services too.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <unistd.h>

#include <string>
#include <thread>
#include <vector>

#include "fld/menus.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "services_util.h"

namespace {

Fl_Double_Window *g_win;
Fl_Choice *g_scope;
Fl_Input *g_search;
Fl_Check_Button *g_running_only;
class ServiceList;
ServiceList *g_list;
Fl_Text_Buffer *g_status_buf;
Fl_Box *g_status;
Fl_Button *g_start, *g_stop, *g_restart, *g_enable, *g_disable;
svc::Init g_init = svc::Init::Unknown;
std::vector<svc::Service> g_all;
std::vector<int> g_shown;
bool g_busy = false;

bool user_scope() { return g_init == svc::Init::Systemd && g_scope->value() == 1; }

void status(const std::string &s) {
  static std::string keep;
  keep = s;
  g_status->label(keep.c_str());
  g_status->redraw();
}

class ServiceList : public Fl_Hold_Browser {
public:
  ServiceList(int x, int y, int w, int h) : Fl_Hold_Browser(x, y, w, h) {
    format_char(0);
    textsize(13);
  }
  void item_draw(void *item, int X, int Y, int W, int H) const override {
    int line = lineno(item);
    if (line < 1 || line > (int)g_shown.size()) return;
    const svc::Service &s = g_all[(size_t)g_shown[(size_t)line - 1]];
    bool sel = item_selected(item);
    const fld::Palette &p = fld::palette();
    Fl_Color fg = sel ? fl_contrast(p.fg, selection_color()) : p.fg;
    // a dot: green running, red failed, grey stopped
    Fl_Color dot = s.failed() ? fl_rgb_color(0xd3, 0x2f, 0x2f) : s.running() ? fl_rgb_color(0x43, 0xa0, 0x47) : p.fg_dim;
    fl_color(dot);
    fl_pie(X + 6, Y + H / 2 - 5, 10, 10, 0, 360);
    fl_font(FL_HELVETICA_BOLD, textsize());
    fl_color(fg);
    fl_draw(s.name.c_str(), X + 24, Y, 200, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    fl_font(FL_HELVETICA, textsize());
    fl_color(sel ? fg : (s.failed() ? dot : p.fg));
    fl_draw(svc::state_label(s).c_str(), X + 230, Y, 80, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    fl_color(sel ? fg : p.fg_dim);
    std::string en = s.enabled.empty() ? (g_init == svc::Init::OpenRC ? "manual" : "") : s.enabled;
    fl_draw(en.c_str(), X + 316, Y, 120, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    fl_color(fg);
    fl_draw(s.description.c_str(), X + 442, Y, W - 444, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  }
  int item_height(void *) const override { return textsize() + 8; }
};

const svc::Service *selected() {
  int i = g_list->value() - 1;
  return i >= 0 && i < (int)g_shown.size() ? &g_all[(size_t)g_shown[(size_t)i]] : nullptr;
}

void update_buttons() {
  const svc::Service *s = selected();
  bool ok = s && !g_busy;
  for (Fl_Button *b : {g_start, g_stop, g_restart, g_enable, g_disable}) ok ? b->activate() : b->deactivate();
  if (!s) return;
  if (s->running()) g_start->deactivate();
  else g_stop->deactivate(), g_restart->deactivate();
  bool enabled = g_init == svc::Init::OpenRC ? !s->enabled.empty() : s->enabled == "enabled";
  if (enabled) g_enable->deactivate();
  else g_disable->deactivate();
  if (s->enabled == "static" || s->enabled == "masked" || s->enabled == "indirect" || s->enabled == "generated")
    g_enable->deactivate(), g_disable->deactivate();
}

void show_status() {
  const svc::Service *s = selected();
  update_buttons();
  if (!s) {
    g_status_buf->text("");
    return;
  }
  std::string out;
  if (g_init == svc::Init::Systemd) {
    std::vector<std::string> cmd = {"systemctl"};
    if (user_scope()) cmd.push_back("--user");
    for (const char *a : {"status", "--no-pager", "-n", "20", "--lines=20"}) cmd.push_back(a);
    cmd.push_back(s->name + ".service");
    fld::run_capture(cmd, out);
  } else {
    fld::run_capture({"rc-service", s->name, "status"}, out);
    std::string desc;
    if (fld::read_file("/etc/init.d/" + s->name, desc, 64 * 1024)) {
      // description="..." in the script
      size_t p = desc.find("description=");
      if (p != std::string::npos) {
        size_t a = desc.find('"', p), b = a == std::string::npos ? a : desc.find('"', a + 1);
        if (b != std::string::npos) out = desc.substr(a + 1, b - a - 1) + "\n\n" + out;
      }
    }
  }
  g_status_buf->text(out.c_str());
}

void fill() {
  std::string sel = selected() ? selected()->name : "";
  std::string q = fld::lower(g_search->value());
  g_shown.clear();
  g_list->clear();
  for (size_t i = 0; i < g_all.size(); i++) {
    const svc::Service &s = g_all[i];
    if (g_running_only->value() && !s.running() && !s.failed()) continue;
    if (!q.empty() && fld::lower(s.name + " " + s.description).find(q) == std::string::npos) continue;
    g_shown.push_back((int)i);
    g_list->add("");
    if (s.name == sel) g_list->select(g_list->size());
  }
  int running = 0, failed = 0;
  for (auto &s : g_all) running += s.running(), failed += s.failed();
  status(std::to_string(g_all.size()) + " services, " + std::to_string(running) + " running" +
         (failed ? ", " + std::to_string(failed) + " failed" : ""));
  show_status();
}

void load() {
  std::string out, files;
  if (g_init == svc::Init::Systemd) {
    std::vector<std::string> base = {"systemctl"};
    if (user_scope()) base.push_back("--user");
    std::vector<std::string> a = base, b = base;
    for (const char *x : {"list-units", "--type=service", "--all", "--plain", "--no-legend", "--no-pager"}) a.push_back(x);
    for (const char *x : {"list-unit-files", "--type=service", "--plain", "--no-legend", "--no-pager"}) b.push_back(x);
    fld::run_capture(a, out);
    fld::run_capture(b, files);
    g_all = svc::parse_systemctl_units(out);
    svc::merge_unit_files(g_all, files);
  } else if (g_init == svc::Init::OpenRC) {
    fld::run_capture({"rc-status", "--all", "--nocolor"}, out);
    fld::run_capture({"rc-update", "show", "-v"}, files);
    g_all = svc::parse_rc_status(out);
    svc::merge_rc_update(g_all, files);
  }
  fill();
}

// runs an action in a thread (polkit may ask for a password meanwhile)
void act(const std::string &action) {
  const svc::Service *s = selected();
  if (!s || g_busy) return;
  std::string name = s->name;
  if ((action == "stop" || action == "disable") &&
      (name == "dbus" || name == "systemd-logind" || name == "elogind" || name == "greetd" || name == "display-manager")) {
    if (fl_choice("Stopping %s can end this session. Go on?", "Cancel", "Go On", nullptr, name.c_str()) != 1) return;
  }
  std::vector<std::string> cmd;
  if (g_init == svc::Init::Systemd) {
    cmd = {"systemctl"};
    if (user_scope()) cmd.push_back("--user");
    cmd.push_back(action);
    if (action == "enable" || action == "disable") cmd.push_back("--now");
    cmd.push_back(name + ".service");
    // starting a service that is enabled with --now is what the user wants;
    // disabling with --now stops it too
  } else {
    cmd = {"pkexec", std::string(FLD_LIBEXECDIR) + "/fl-services-helper", action, name};
  }
  g_busy = true;
  update_buttons();
  status(action + " " + name + "…");
  fl_cursor(FL_CURSOR_WAIT);
  std::thread([cmd, name, action] {
    std::string out;
    int rc = fld::run_capture(cmd, out);
    Fl::lock();
    struct Done {
      int rc;
      std::string out, name, action;
    };
    Fl::awake(
        [](void *d) {
          Done *r = (Done *)d;
          g_busy = false;
          fl_cursor(FL_CURSOR_DEFAULT);
          load();
          if (r->rc != 0) {
            std::string m = fld::trim(r->out);
            if (m.empty()) m = "it did not work";
            status(r->action + " " + r->name + " failed: " + m);
            // the reason often is in the status
          } else {
            status(r->action + " " + r->name + ": done");
          }
          delete r;
        },
        new Done{rc, out, name, action});
    Fl::unlock();
  }).detach();
}

} // namespace

int main(int argc, char **argv) {
  fld::init_app("fl-services", argc, argv);
  Fl::lock();
  g_init = svc::detect_init();
  const int W = 1000, H = 640;
  g_win = new Fl_Double_Window(W, H, "Services");
  int y = 8;
  g_scope = new Fl_Choice(84, y, 180, 28, "Services:");
  g_scope->add("System");
  if (g_init == svc::Init::Systemd) g_scope->add("Mine (user)");
  g_scope->value(0);
  g_scope->callback([](Fl_Widget *, void *) { load(); });
  g_search = new Fl_Input(330, y, 300, 28, "Find:");
  g_search->when(FL_WHEN_CHANGED);
  g_search->callback([](Fl_Widget *, void *) { fill(); });
  g_running_only = new Fl_Check_Button(644, y, 160, 28, "Only running");
  g_running_only->callback([](Fl_Widget *, void *) { fill(); });
  Fl_Button *reload = new Fl_Button(W - 100, y, 92, 28, "Reload");
  reload->callback([](Fl_Widget *, void *) { load(); });
  y += 36;
  int bx = 8;
  auto btn = [&](const char *l, const char *tip, const char *action) {
    Fl_Button *b = new Fl_Button(bx, y, 96, 30, l);
    b->tooltip(tip);
    b->callback([](Fl_Widget *, void *a) { act((const char *)a); }, (void *)action);
    bx += 100;
    return b;
  };
  g_start = btn("Start", "Start the service now", "start");
  g_stop = btn("Stop", "Stop the service now", "stop");
  g_restart = btn("Restart", "Stop and start it again", "restart");
  bx += 12;
  g_enable = btn("Enable", g_init == svc::Init::OpenRC ? "Start it at boot (default runlevel)" : "Start it at boot, and now",
                 "enable");
  g_disable = btn("Disable", g_init == svc::Init::OpenRC ? "Do not start it at boot" : "Do not start it at boot; stop it",
                  "disable");
  y += 38;
  Fl_Tile *tile = new Fl_Tile(0, y, W, H - y - 26);
  int lh = (H - y - 26) * 3 / 5;
  g_list = new ServiceList(0, y, W, lh);
  g_list->callback([](Fl_Widget *, void *) {
    show_status();
    if (Fl::event_clicks() && Fl::event() == FL_RELEASE) {
      const svc::Service *s = selected();
      if (s) act(s->running() ? "restart" : "start");
    }
  });
  g_status_buf = new Fl_Text_Buffer();
  Fl_Text_Display *d = new Fl_Text_Display(0, y + lh, W, H - y - 26 - lh);
  d->buffer(g_status_buf);
  d->textfont(FL_COURIER);
  d->textsize(12);
  tile->end();
  g_status = new Fl_Box(8, H - 26, W - 16, 24);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->end();
  g_win->resizable(tile);
  g_win->size_range(700, 400);
  fld::set_window_icon(g_win, "fl-services");
  g_win->show();
  if (g_init == svc::Init::Unknown) {
    status("Neither systemd nor OpenRC runs here: services cannot be managed.");
  } else {
    load();
  }
  return Fl::run();
}
