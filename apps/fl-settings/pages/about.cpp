// About: system information
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>

#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

std::string os_name() {
  Ini r;
  if (!r.load("/etc/os-release")) r.load("/usr/lib/os-release");
  std::string n = r.get("", "PRETTY_NAME", "Linux");
  if (n.size() >= 2 && n.front() == '"') n = n.substr(1, n.size() - 2);
  return n;
}

std::string cpu_name() {
  std::string t, name;
  int count = 0;
  if (read_file("/proc/cpuinfo", t))
    for (auto &l : split(t, '\n')) {
      if (starts_with(l, "model name") && name.empty()) name = trim(l.substr(l.find(':') + 1));
      if (starts_with(l, "processor")) count++;
    }
  if (name.empty()) name = "Unknown processor";
  return name + (count > 1 ? " × " + std::to_string(count) : "");
}

std::string memory() {
  std::string t;
  if (read_file("/proc/meminfo", t))
    for (auto &l : split(t, '\n'))
      if (starts_with(l, "MemTotal:")) return human_size(strtoull(l.c_str() + 9, nullptr, 10) * 1024ULL);
  return "?";
}

std::string disk() {
  struct statvfs v;
  if (statvfs("/", &v) != 0) return "?";
  unsigned long long total = (unsigned long long)v.f_blocks * v.f_frsize;
  unsigned long long avail = (unsigned long long)v.f_bavail * v.f_frsize;
  return human_size(total) + " (" + human_size(avail) + " free)";
}

std::string graphics() {
  std::vector<std::string> v;
  for (auto &c : list_dir("/sys/class/drm")) {
    if (c.find('-') != std::string::npos || !starts_with(c, "card")) continue;
    char buf[512];
    ssize_t n = readlink(("/sys/class/drm/" + c + "/device/driver").c_str(), buf, sizeof buf - 1);
    if (n > 0) {
      buf[n] = 0;
      v.push_back(basename(buf));
    }
  }
  return v.empty() ? "?" : join(v, ", ");
}

class AboutPage : public Fl_Group {
public:
  std::string info;
  AboutPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, N_("About"), cy);
    resizable(g);
    Fl_Box *logo = new Fl_Box(X + 24, cy, 96, 96);
    logo->image(icon("fldesktop", 96));
    Fl_Box *name = new Fl_Box(X + 136, cy + 20, 400, 30, "fldesktop " FLD_VERSION);
    name->labelfont(FL_HELVETICA_BOLD);
    name->labelsize(22);
    name->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    Fl_Box *sub = new Fl_Box(X + 136, cy + 52, 400, 24, _("A lightweight Wayland desktop built with FLTK"));
    sub->labelcolor(palette().fg_dim);
    sub->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    struct utsname u;
    uname(&u);
    char host[256] = "";
    gethostname(host, sizeof host - 1);
    std::vector<std::pair<std::string, std::string>> rows = {
        {"Device name", host},
        {"Operating system", os_name()},
        {"Kernel", std::string(u.sysname) + " " + u.release + " (" + u.machine + ")"},
        {"Processor", cpu_name()},
        {"Memory", memory()},
        {"Disk", disk()},
        {"Graphics driver", graphics()},
        {"Session", getenv_or("XDG_SESSION_TYPE", "?") + " (" + getenv_or("XDG_CURRENT_DESKTOP", "?") + ")"},
        {"FLTK", std::to_string(FL_MAJOR_VERSION) + "." + std::to_string(FL_MINOR_VERSION) + "." +
                     std::to_string(FL_PATCH_VERSION)},
    };
    Form f(g, X + 24, cy + 120, std::min(W - 48, 640), 170);
    int wx, wy, ww;
    for (auto &r : rows) {
      f.row((r.first + ":").c_str(), 24, wx, wy, ww);
      Fl_Box *v = new Fl_Box(wx, wy, ww, 24);
      v->copy_label(escape_at(r.second).c_str());
      v->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      info += r.first + ": " + r.second + "\n";
    }
    f.space(6);
    f.row("", 30, wx, wy, ww);
    Fl_Button *copy = new Fl_Button(wx, wy, 180, 30, _("Copy to Clipboard"));
    copy->callback([](Fl_Widget *, void *d) {
      AboutPage *p = (AboutPage *)d;
      Fl::copy(p->info.c_str(), (int)p->info.size(), 1);
    }, this);
    // the full details: fl-sysinfo, when it is installed
    if (!which("fl-sysinfo").empty()) {
      Fl_Button *more = new Fl_Button(wx + 190, wy, 200, 30, _("System Information…"));
      more->tooltip(_("Processor, memory, graphics, disks, devices, batteries and temperatures in detail"));
      more->callback([](Fl_Widget *, void *) { spawn({"fl-sysinfo"}); });
    }
    g->end();
    end();
  }
};

Fl_Group *create(int x, int y, int w, int h) { return new AboutPage(x, y, w, h); }

} // namespace

PageInfo page_about() {
  return {"about", _("About"), "help-about", "System", _("about system information version hardware cpu memory"), create};
}

} // namespace fld
