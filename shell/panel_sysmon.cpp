// fl-panel: a small system monitor ([Panel] SystemMonitor=true): the
// processor and memory use of the last minute as two graphs. A click
// opens the task manager.
#include "fld/i18n.h"
#include <FL/fl_draw.H>

#include <deque>
#include <string>

#include "applets_util.h"
#include "fld/ini.h"
#include "fld/util.h"
#include "panel.h"

namespace {

class SysMon : public fld::FlatButton {
public:
  std::deque<double> cpu, mem;
  applets::CpuTimes last;
  std::string tip;
  SysMon(int x, int y, int w, int h) : fld::FlatButton(x, y, w, h) {}
  void draw() override {
    fld::FlatButton::draw();
    int gx = x() + 4, gw = (w() - 12) / 2, gy = y() + 6, gh = h() - 12;
    graph(cpu, gx, gy, gw, gh, fld::palette().accent);
    graph(mem, gx + gw + 4, gy, gw, gh, fl_rgb_color(0x4c, 0xaf, 0x50));
  }
  void graph(const std::deque<double> &v, int X, int Y, int W, int H, Fl_Color c) {
    fl_color(fl_color_average(panel_fg(), panel_bg(), 0.18f));
    fl_rectf(X, Y, W, H);
    fl_color(c);
    int n = (int)v.size();
    for (int i = 0; i < n; i++) {
      int bx = X + W - n + i;
      if (bx < X) continue;
      int bh = (int)(v[(size_t)i] * H + 0.5);
      if (bh > 0) fl_rectf(bx, Y + H - bh, 1, bh);
    }
  }
  void sample() {
    std::string stat, meminfo;
    fld::read_file("/proc/stat", stat, 4096);
    applets::CpuTimes now;
    double c = 0;
    if (applets::parse_proc_stat(stat, now)) {
      if (last.total) c = applets::cpu_usage(last, now);
      last = now;
    }
    fld::read_file("/proc/meminfo", meminfo, 8192);
    unsigned long long total = 0;
    double m = applets::memory_usage(meminfo, &total);
    cpu.push_back(c);
    mem.push_back(m);
    size_t keep = (size_t)std::max(8, (w() - 12) / 2);
    while (cpu.size() > keep) cpu.pop_front();
    while (mem.size() > keep) mem.pop_front();
    tip = "Processor " + std::to_string((int)(c * 100 + 0.5)) + "%, memory " + std::to_string((int)(m * 100 + 0.5)) +
          "% of " + fld::human_size(total * 1024);
    tooltip(tip.c_str());
    redraw();
  }
};

SysMon *g_mon;

void tick(void *) {
  g_mon->sample();
  Fl::repeat_timeout(2.0, tick);
}

} // namespace

fld::FlatButton *sysmon_create(bool on) {
  g_mon = new SysMon(0, 0, 60, PANEL_H - 4);
  g_mon->color(panel_bg());
  g_mon->hover_color(panel_hover());
  g_mon->callback([](Fl_Widget *, void *) { fld::spawn({"fl-taskmgr"}); });
  panel_context_menu(g_mon, [](fld::Menu &m) {
    m.item(_("Task Manager…"), [] { fld::spawn({"fl-taskmgr"}); });
    m.item(_("Hide the System Monitor"), [] {
      fld::Ini c;
      c.load(fld::fld_config_dir() + "/panel.ini");
      c.set_bool("Panel", "SystemMonitor", false);
      c.save(fld::fld_config_dir() + "/panel.ini");
    }).divider();
    panel_menu_items(m);
  });
  if (!on) {
    g_mon->hide();
    return g_mon;
  }
  g_mon->sample();
  Fl::add_timeout(2.0, tick);
  return g_mon;
}
