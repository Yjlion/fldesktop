// Sharing: the computer's name on the network, network discovery (Avahi,
// mDNS) and the services it announces. The changes go through
// pkexec fl-settings-helper.
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Input.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <functional>
#include <thread>
#include <unistd.h>
#include <vector>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"
#include "sharing_util.h"

namespace fld {
namespace {

// runs the helper through pkexec in a thread; done() in the FLTK thread
void run_helper(std::vector<std::string> args, std::function<void(int, const std::string &)> done) {
  std::vector<std::string> argv = {"pkexec", settings_helper_path()};
  argv.insert(argv.end(), args.begin(), args.end());
  struct Res { int code; std::string out; std::function<void(int, const std::string &)> done; };
  std::thread([argv, done] {
    Res *r = new Res{0, "", done};
    r->code = run_capture_input(argv, "", r->out, true);
    Fl::awake([](void *d) {
      Res *r = (Res *)d;
      r->done(r->code, r->out);
      delete r;
    }, r);
  }).detach();
}

std::string current_hostname() {
  char b[256] = {0};
  gethostname(b, sizeof b - 1);
  return b;
}

bool avahi_installed() {
  return file_exists("/usr/sbin/avahi-daemon") || file_exists("/usr/bin/avahi-daemon");
}
bool avahi_running() {
  return file_exists("/run/avahi-daemon/socket") || file_exists("/var/run/avahi-daemon/socket");
}

class SharingPage : public Fl_Group {
public:
  Fl_Input *name;
  Fl_Button *apply;
  Fl_Box *local;
  Fl_Check_Button *discovery;
  std::vector<std::pair<const sharing::Published *, Fl_Check_Button *>> pubs;
  bool busy = false;

  SharingPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Sharing", cy);
    resizable(g);
    Form f(g, X + 24, cy, std::min(W - 48, 640));
    int wx, wy, ww;
    f.heading(_("This computer"));
    f.row(_("Computer name:"), 28, wx, wy, ww);
    name = new Fl_Input(wx, wy, 220, 28);
    name->value(current_hostname().c_str());
    name->tooltip(_("Letters, digits and '-' (and '.' for a full name): how other computers and devices see this one"));
    name->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY);
    name->callback([](Fl_Widget *, void *d) {
      SharingPage *p = (SharingPage *)d;
      if (Fl::event_key() == FL_Enter) p->set_name();
      else p->update();
    }, this);
    apply = new Fl_Button(wx + 228, wy, 90, 28, _("Apply"));
    apply->callback([](Fl_Widget *, void *d) { ((SharingPage *)d)->set_name(); }, this);
    f.row("", 22, wx, wy, ww);
    local = new Fl_Box(wx, wy, ww, 22);
    local->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    local->labelcolor(palette().fg_dim);

    f.heading(_("Network discovery"));
    f.row("", 26, wx, wy, ww);
    discovery = new fld::CheckButton(wx, wy, ww, 26, _("Find printers, shares and computers, and be found (mDNS)"));
    discovery->tooltip(_("The Avahi daemon: names like computer.local, and the services of the network"));
    discovery->callback([](Fl_Widget *, void *d) { ((SharingPage *)d)->set_discovery(); }, this);
    if (!avahi_installed()) f.note(_("Avahi (avahi-daemon) is not installed."));

    f.heading(_("Announce on the network"));
    for (auto &p : sharing::published()) {
      f.row("", 26, wx, wy, ww);
      Fl_Check_Button *b = new fld::CheckButton(wx, wy, ww, 26, p.label);
      b->callback([](Fl_Widget *w, void *d) { ((SharingPage *)d)->set_published((Fl_Check_Button *)w); }, this);
      pubs.push_back({&p, b});
    }
    f.note(_("Others see these services in their file managers and remote desktop programs.\n"
           "Announcing a service does not start it, and the firewall may still block it."), 2);
    g->end();
    end();
    update();
  }

  void update() {
    std::string n = trim(name->value());
    bool changed = n != current_hostname();
    bool valid = sharing::valid_hostname(n);
    changed && valid && !busy ? apply->activate() : apply->deactivate();
    // mDNS uses the first label: box.example.org is box.local
    std::string host = current_hostname(), short_name = host.substr(0, host.find('.'));
    std::string l = !valid ? "Use letters, digits, '-' and '.' only."
                    : avahi_running() ? "Others find it as " + short_name + ".local"
                                      : "Turn on network discovery to be found as " + short_name + ".local";
    local->copy_label(escape_at(l).c_str());
    local->redraw();
    discovery->value(avahi_running());
    avahi_installed() && !busy ? discovery->activate() : discovery->deactivate();
    bool avahi_dir = is_dir("/etc/avahi/services");
    for (auto &pb : pubs) {
      pb.second->value(file_exists(sharing::service_path(pb.first->id)));
      bool have = std::any_of(pb.first->needs.begin(), pb.first->needs.end(),
                              [](const char *p) { return file_exists(p); });
      std::string tip = have ? std::string("Announces ") + pb.first->type + " on port " +
                                   std::to_string(pb.first->port)
                             : "Not installed on this computer";
      pb.second->copy_tooltip(tip.c_str());
      // turning one off stays possible when the program went away
      (avahi_dir && (have || pb.second->value()) && !busy) ? pb.second->activate() : pb.second->deactivate();
    }
  }

  void run(const char *what, std::vector<std::string> args) {
    if (which("pkexec").empty()) {
      fl_alert(_("%s needs pkexec (polkit)."), what);
      update();
      return;
    }
    busy = true;
    update();
    std::string w = what;
    run_helper(args, [this, w](int code, const std::string &out) {
      busy = false;
      // 126 / 127: the authentication was cancelled
      if (code != 0 && code != 126 && code != 127) fl_alert(_("%s failed:\n%s"), w.c_str(), trim(out).c_str());
      // the daemon takes a moment to come up or go away
      Fl::add_timeout(0.6, [](void *d) { ((SharingPage *)d)->update(); }, this);
      update();
    });
  }

  void set_name() {
    std::string n = trim(name->value());
    if (!sharing::valid_hostname(n) || n == current_hostname()) return;
    run("Changing the computer name", {"hostname", n});
  }
  void set_discovery() { run("Changing network discovery", {"avahi", discovery->value() ? "on" : "off"}); }
  void set_published(Fl_Check_Button *b) {
    for (auto &pb : pubs)
      if (pb.second == b) run("Changing what is announced", {"publish", pb.first->id, b->value() ? "on" : "off"});
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new SharingPage(x, y, w, h); }

} // namespace

PageInfo page_sharing() {
  return {"sharing", _("Sharing"), "network-workgroup", "Network",
          _("sharing computer name hostname mdns avahi bonjour zeroconf discovery local ssh sftp vnc samba announce"),
          create};
}

} // namespace fld
