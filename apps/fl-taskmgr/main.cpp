// fl-taskmgr: task manager for fldesktop (processes and performance graphs)
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Tabs.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unistd.h>
#include <vector>

#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "procinfo.h"

using procinfo::Process;

static procinfo::Sampler g_sampler;

struct Settings {
  double interval = 1.0;
  bool all_users = true, tree = false, kernel = false;
  int sort_col = 3; // CPU
  bool sort_desc = true;
} S;

static std::string settings_path() { return fld::fld_config_dir() + "/taskmgr.ini"; }

static void load_settings() {
  fld::Ini c;
  c.load(settings_path());
  S.interval = std::max(0.25, std::min(10.0, atof(c.get("TaskManager", "Interval", "1").c_str())));
  S.all_users = c.get_bool("TaskManager", "AllUsers", true);
  S.tree = c.get_bool("TaskManager", "Tree", false);
  S.kernel = c.get_bool("TaskManager", "KernelThreads", false);
  S.sort_col = std::max(0, std::min(8, c.get_int("TaskManager", "SortColumn", 3)));
  S.sort_desc = c.get_bool("TaskManager", "SortDescending", true);
}

static void save_settings() {
  fld::Ini c;
  c.load(settings_path());
  char b[32];
  snprintf(b, sizeof b, "%g", S.interval);
  c.set("TaskManager", "Interval", b);
  c.set_bool("TaskManager", "AllUsers", S.all_users);
  c.set_bool("TaskManager", "Tree", S.tree);
  c.set_bool("TaskManager", "KernelThreads", S.kernel);
  c.set_int("TaskManager", "SortColumn", S.sort_col);
  c.set_bool("TaskManager", "SortDescending", S.sort_desc);
  c.save(settings_path());
}

static std::string fmt(const char *f, double v) {
  char b[64];
  snprintf(b, sizeof b, f, v);
  return b;
}

// ---------------------------------------------------------------------------
// Process table

static const char *kCols[] = {"Name", "PID", "User", "CPU %", "Memory", "Threads", "State", "Nice", "Command"};
static const int kNumCols = 9;

class ProcTable : public Fl_Table_Row {
public:
  struct Row {
    Process p;
    int depth = 0;
  };
  std::vector<Row> items;
  std::string filter;
  pid_t selected = 0;
  std::function<void(pid_t)> on_menu;
  std::function<void()> on_select;

  ProcTable(int x, int y, int w, int h) : Fl_Table_Row(x, y, w, h) {
    type(SELECT_SINGLE);
    cols(kNumCols);
    col_header(1);
    col_header_height(26);
    row_height_all(24);
    col_resize(1);
    const int widths[] = {220, 70, 90, 70, 90, 70, 90, 50, 400};
    for (int i = 0; i < kNumCols; i++) col_width(i, widths[i]);
    color(fld::palette().bg2);
    selection_color(fld::palette().accent);
    end();
    callback([](Fl_Widget *w, void *) { static_cast<ProcTable *>(w)->clicked(); });
    when(FL_WHEN_RELEASE | FL_WHEN_CHANGED);
  }

  static bool less(const Process &a, const Process &b, int col) {
    switch (col) {
      case 0: return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
      case 1: return a.pid < b.pid;
      case 2: return a.user < b.user;
      case 3: return a.cpu < b.cpu;
      case 4: return a.rss < b.rss;
      case 5: return a.threads < b.threads;
      case 6: return a.state < b.state;
      case 7: return a.nice < b.nice;
      default: return a.cmdline < b.cmdline;
    }
  }

  bool visible(const Process &p) const {
    if (!S.kernel && p.kernel_thread) return false;
    if (!S.all_users && p.uid != getuid()) return false;
    if (!filter.empty()) {
      std::string f = fld::lower(filter);
      if (fld::lower(p.name).find(f) == std::string::npos && fld::lower(p.cmdline).find(f) == std::string::npos &&
          std::to_string(p.pid) != f && p.user != f)
        return false;
    }
    return true;
  }

  void rebuild() {
    std::vector<Process> all;
    for (auto &p : g_sampler.processes())
      if (visible(p)) all.push_back(p);
    auto cmp = [](const Process &a, const Process &b) {
      bool l = S.sort_desc ? less(b, a, S.sort_col) : less(a, b, S.sort_col);
      if (!l && !(S.sort_desc ? less(a, b, S.sort_col) : less(b, a, S.sort_col))) return a.pid < b.pid;
      return l;
    };
    items.clear();
    if (S.tree && filter.empty()) {
      std::set<pid_t> present;
      for (auto &p : all) present.insert(p.pid);
      std::map<pid_t, std::vector<Process>> children;
      std::vector<Process> roots;
      for (auto &p : all) {
        if (present.count(p.ppid) && p.ppid != p.pid) children[p.ppid].push_back(p);
        else roots.push_back(p);
      }
      std::sort(roots.begin(), roots.end(), cmp);
      std::function<void(const Process &, int)> add = [&](const Process &p, int depth) {
        items.push_back({p, depth});
        auto it = children.find(p.pid);
        if (it == children.end()) return;
        std::sort(it->second.begin(), it->second.end(), cmp);
        for (auto &c : it->second) add(c, depth + 1);
      };
      for (auto &r : roots) add(r, 0);
    } else {
      std::sort(all.begin(), all.end(), cmp);
      for (auto &p : all) items.push_back({p, 0});
    }
    int top = row_position();
    rows_changed();
    row_position(std::min(top, std::max(0, (int)items.size() - 1)));
  }

  void rows_changed() {
    Fl_Table_Row::rows((int)items.size());
    select_all_rows(0);
    for (size_t i = 0; i < items.size(); i++)
      if (items[i].p.pid == selected) select_row((int)i, 1);
    redraw();
  }

  const Process *selected_process() const {
    for (auto &r : items)
      if (r.p.pid == selected) return &r.p;
    return nullptr;
  }

  void clicked() {
    int R = callback_row(), C = callback_col();
    TableContext ctx = callback_context();
    if (ctx == CONTEXT_COL_HEADER && Fl::event() == FL_RELEASE && Fl::event_button() == FL_LEFT_MOUSE) {
      if (S.sort_col == C) S.sort_desc = !S.sort_desc;
      else {
        S.sort_col = C;
        S.sort_desc = C == 3 || C == 4 || C == 5; // numbers: biggest first
      }
      save_settings();
      rebuild();
      return;
    }
    if (ctx == CONTEXT_CELL && R >= 0 && R < (int)items.size()) {
      selected = items[(size_t)R].p.pid;
      if (on_select) on_select();
      if (Fl::event() == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE && on_menu) {
        select_all_rows(0);
        select_row(R, 1);
        on_menu(selected);
      }
    }
  }

  int handle(int e) override {
    if (e == FL_KEYBOARD && !items.empty()) {
      int k = Fl::event_key();
      int cur = -1;
      for (int i = 0; i < (int)items.size(); i++)
        if (items[(size_t)i].p.pid == selected) cur = i;
      int r1, r2, c1, c2;
      visible_cells(r1, r2, c1, c2);
      int page = std::max(1, r2 - r1 - 1), n = (int)items.size(), next = -2;
      switch (k) {
        case FL_Down: next = cur < 0 ? 0 : std::min(n - 1, cur + 1); break;
        case FL_Up: next = cur < 0 ? 0 : std::max(0, cur - 1); break;
        case FL_Page_Down: next = std::min(n - 1, std::max(0, cur) + page); break;
        case FL_Page_Up: next = std::max(0, cur - page); break;
        case FL_Home: next = 0; break;
        case FL_End: next = n - 1; break;
      }
      if (next >= 0) {
        selected = items[(size_t)next].p.pid;
        select_all_rows(0);
        select_row(next, 1);
        if (next < r1 + 1 || next >= r2) row_position(next <= r1 ? next : std::max(0, next - page));
        if (on_select) on_select();
        return 1;
      }
    }
    return Fl_Table_Row::handle(e);
  }

  std::string cell_text(const Row &r, int c) const {
    const Process &p = r.p;
    switch (c) {
      case 0: return std::string((size_t)r.depth * 3, ' ') + p.name;
      case 1: return std::to_string(p.pid);
      case 2: return p.user;
      case 3: return fmt("%.1f", p.cpu);
      case 4: return p.kernel_thread ? "" : procinfo::human_bytes((double)p.rss);
      case 5: return std::to_string(p.threads);
      case 6: return procinfo::state_name(p.state);
      case 7: return std::to_string(p.nice);
      default: return p.cmdline.empty() ? "[" + p.name + "]" : p.cmdline;
    }
  }

  void draw_cell(TableContext ctx, int R, int C, int X, int Y, int W, int H) override {
    const fld::Palette &pal = fld::palette();
    switch (ctx) {
      case CONTEXT_STARTPAGE: fl_font(FL_HELVETICA, 13); return;
      case CONTEXT_COL_HEADER: {
        fl_push_clip(X, Y, W, H);
        fl_draw_box(FL_FLAT_BOX, X, Y, W, H, pal.bg);
        fl_color(pal.border);
        fl_xyline(X, Y + H - 1, X + W);
        fl_yxline(X + W - 1, Y + 4, Y + H - 5);
        fl_color(pal.fg);
        fl_font(FL_HELVETICA_BOLD, 12);
        std::string l = kCols[C];
        if (C == S.sort_col) l += S.sort_desc ? "  ▾" : "  ▴";
        fl_draw(l.c_str(), X + 6, Y, W - 12, H, C == 0 || C == 2 || C == 6 || C == 8 ? FL_ALIGN_LEFT : FL_ALIGN_RIGHT);
        fl_pop_clip();
        return;
      }
      case CONTEXT_CELL: {
        if (R >= (int)items.size()) return;
        const Row &r = items[(size_t)R];
        bool sel = row_selected(R);
        fl_push_clip(X, Y, W, H);
        Fl_Color bg = sel ? pal.accent : (R % 2 ? fl_color_average(pal.bg2, pal.bg, 0.6f) : pal.bg2);
        fl_color(bg);
        fl_rectf(X, Y, W, H);
        // a heat background for busy processes
        if (C == 3 && !sel && r.p.cpu >= 1) {
          double f = std::min(1.0, r.p.cpu / 100.0);
          fl_color(fl_color_average(fl_rgb_color(0xf5, 0xa6, 0x23), bg, (float)(0.2 + 0.6 * f)));
          fl_rectf(X, Y, W, H);
        }
        fl_color(sel ? pal.accent_fg : (r.p.state == 'Z' || r.p.state == 'T') ? pal.fg_dim : pal.fg);
        fl_font(FL_HELVETICA, 13);
        std::string t = fld::escape_at(cell_text(r, C));
        Fl_Align al = C == 0 || C == 2 || C == 6 || C == 8 ? FL_ALIGN_LEFT : FL_ALIGN_RIGHT;
        fl_draw(t.c_str(), X + 6, Y, W - 12, H, al | FL_ALIGN_CLIP, nullptr, 0);
        fl_pop_clip();
        return;
      }
      default: return;
    }
  }
};

// ---------------------------------------------------------------------------
// History graph

class HistoryGraph : public Fl_Widget {
public:
  struct Series {
    std::string name;
    Fl_Color color;
    std::deque<double> values;
  };
  std::vector<Series> series;
  std::string title, value_text;
  double fixed_max = 0; // 0: scale to the data
  bool bytes = false;   // label the scale as a rate
  static const int N = 60;

  HistoryGraph(int x, int y, int w, int h, const char *t) : Fl_Widget(x, y, w, h), title(t) {}

  void add_series(const std::string &n, Fl_Color c) { series.push_back({n, c, {}}); }
  void push(size_t i, double v) {
    if (i >= series.size()) return;
    auto &d = series[i].values;
    d.push_back(v);
    while (d.size() > (size_t)N) d.pop_front();
  }

  void draw() override {
    const fld::Palette &pal = fld::palette();
    fl_push_clip(x(), y(), w(), h());
    fl_color(pal.bg);
    fl_rectf(x(), y(), w(), h());
    // header
    fl_color(pal.fg);
    fl_font(FL_HELVETICA_BOLD, 14);
    fl_draw(title.c_str(), x() + 4, y() + 2, w() - 8, 22, FL_ALIGN_LEFT);
    fl_font(FL_HELVETICA, 12);
    fl_color(pal.fg_dim);
    fl_draw(value_text.c_str(), x() + 4, y() + 2, w() - 8, 22, FL_ALIGN_RIGHT | FL_ALIGN_CLIP);
    int gx = x() + 4, gy = y() + 26, gw = w() - 8, gh = h() - 26 - 22;
    if (gw < 10 || gh < 10) { fl_pop_clip(); return; }
    fl_color(pal.bg2);
    fl_rectf(gx, gy, gw, gh);
    double mx = fixed_max;
    if (mx <= 0) {
      mx = 1;
      for (auto &s : series)
        for (double v : s.values) mx = std::max(mx, v);
      // round the scale up to 1, 2 or 5 * 10^n (in KiB/MiB/... units for rates)
      double unit = 1;
      while (bytes && mx >= unit * 1024) unit *= 1024;
      double v = mx / unit, p = 1;
      while (p * 10 < v) p *= 10;
      v = v <= p ? p : v <= 2 * p ? 2 * p : v <= 5 * p ? 5 * p : 10 * p;
      mx = v * unit;
    }
    // grid
    fl_color(fl_color_average(pal.border, pal.bg2, 0.5f));
    for (int i = 1; i < 4; i++) fl_xyline(gx, gy + gh * i / 4, gx + gw - 1);
    for (int i = 1; i < 6; i++) fl_yxline(gx + gw * i / 6, gy, gy + gh - 1);
    // series
    for (auto &s : series) {
      if (s.values.size() < 2) continue;
      // filled area under the line
      fl_color(fl_color_average(s.color, pal.bg2, series.size() > 2 ? 0.08f : 0.25f));
      size_t n = s.values.size();
      for (size_t i = 0; i + 1 < n; i++) {
        int x0 = gx + gw - 1 - (int)((n - 1 - i) * (gw - 1) / (N - 1));
        int x1 = gx + gw - 1 - (int)((n - 2 - i) * (gw - 1) / (N - 1));
        int y0 = gy + gh - 1 - (int)(std::min(1.0, s.values[i] / mx) * (gh - 2));
        int y1 = gy + gh - 1 - (int)(std::min(1.0, s.values[i + 1] / mx) * (gh - 2));
        fl_begin_complex_polygon();
        fl_vertex(x0, gy + gh - 1);
        fl_vertex(x0, y0);
        fl_vertex(x1, y1);
        fl_vertex(x1, gy + gh - 1);
        fl_end_complex_polygon();
      }
      fl_color(s.color);
      fl_line_style(FL_SOLID, 2);
      fl_begin_line();
      for (size_t i = 0; i < n; i++) {
        int px = gx + gw - 1 - (int)((n - 1 - i) * (gw - 1) / (N - 1));
        int py = gy + gh - 1 - (int)(std::min(1.0, s.values[i] / mx) * (gh - 2));
        fl_vertex(px, py);
      }
      fl_end_line();
      fl_line_style(0);
    }
    fl_color(pal.border);
    fl_rect(gx, gy, gw, gh);
    // scale and legend
    fl_font(FL_HELVETICA, 11);
    fl_color(pal.fg_dim);
    std::string top = bytes ? procinfo::human_bytes(mx) + "/s" : fmt("%.0f%%", mx);
    fl_draw(top.c_str(), gx + 3, gy + 1, gw, 14, FL_ALIGN_LEFT | FL_ALIGN_TOP);
    fl_draw("60 s", gx + 3, gy + gh + 2, 60, 18, FL_ALIGN_LEFT);
    int lx = gx + gw;
    for (size_t i = series.size(); i-- > 0;) {
      if (series.size() > 4) break; // per-core graphs: no legend
      int tw = (int)fl_width(series[i].name.c_str());
      lx -= tw + 22;
      fl_color(series[i].color);
      fl_rectf(lx, gy + gh + 7, 10, 10);
      fl_color(pal.fg_dim);
      fl_draw(series[i].name.c_str(), lx + 14, gy + gh + 2, tw + 4, 18, FL_ALIGN_LEFT);
    }
    fl_pop_clip();
  }
};

// ---------------------------------------------------------------------------
// Globals and actions

static Fl_Double_Window *g_win;
static ProcTable *g_table;
static Fl_Box *g_status;
static Fl_Button *g_end_btn;

// Down / Enter in the filter moves into the list and selects the first row
class FilterInput : public Fl_Input {
public:
  using Fl_Input::Fl_Input;
  int handle(int e) override;
};
static FilterInput *g_filter;
static HistoryGraph *g_cpu, *g_mem, *g_net, *g_disk;
static bool g_per_core = true;

static Fl_Color series_color(int i) {
  static const unsigned c[] = {0x3584e4, 0xe66100, 0x2ec27e, 0x9141ac, 0xe5a50a, 0xc01c28, 0x1c71d8, 0x26a269,
                               0x865e3c, 0x33c7de, 0xf66151, 0x613583};
  unsigned v = c[i % 12];
  return fl_rgb_color((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

static void setup_cpu_series() {
  g_cpu->series.clear();
  int n = g_sampler.cpu_count();
  if (g_per_core && n > 1)
    for (int i = 0; i < n; i++) g_cpu->add_series("CPU " + std::to_string(i), series_color(i));
  else g_cpu->add_series("All CPUs", series_color(0));
}

static void update_status() {
  const procinfo::System &s = g_sampler.system();
  long up = (long)s.uptime;
  char b[256];
  snprintf(b, sizeof b, "Processes: %zu shown, %d total    CPU: %.0f%%    Memory: %s / %s    Load: %.2f %.2f %.2f    Up: %ldd %02ld:%02ld",
           g_table->items.size(), s.procs_total, g_sampler.cpu_percent(), procinfo::human_bytes((double)s.mem.used()).c_str(),
           procinfo::human_bytes((double)s.mem.total).c_str(), s.load[0], s.load[1], s.load[2], up / 86400,
           (up / 3600) % 24, (up / 60) % 60);
  g_status->copy_label(b);
  if (g_table->selected_process()) g_end_btn->activate();
  else g_end_btn->deactivate();
}

static void refresh() {
  g_sampler.sample();
  g_table->rebuild();
  const procinfo::System &s = g_sampler.system();
  // graphs
  if (g_cpu->series.size() != (g_per_core && g_sampler.cpu_count() > 1 ? (size_t)g_sampler.cpu_count() : 1u))
    setup_cpu_series();
  if (g_cpu->series.size() == 1) g_cpu->push(0, g_sampler.cpu_percent());
  else
    for (size_t i = 0; i < g_sampler.core_percent().size(); i++) g_cpu->push(i, g_sampler.core_percent()[i]);
  g_cpu->value_text = fmt("%.0f%%", g_sampler.cpu_percent()) + "  ·  " + std::to_string(g_sampler.cpu_count()) + " CPUs";
  double mem_pct = s.mem.total ? 100.0 * (double)s.mem.used() / (double)s.mem.total : 0;
  double swap_pct = s.mem.swap_total ? 100.0 * (double)s.mem.swap_used() / (double)s.mem.swap_total : 0;
  g_mem->push(0, mem_pct);
  g_mem->push(1, swap_pct);
  g_mem->value_text = procinfo::human_bytes((double)s.mem.used()) + " of " + procinfo::human_bytes((double)s.mem.total) +
                      (s.mem.swap_total ? "  ·  swap " + procinfo::human_bytes((double)s.mem.swap_used()) : "");
  g_net->push(0, g_sampler.net_rx_rate());
  g_net->push(1, g_sampler.net_tx_rate());
  g_net->value_text = "↓ " + procinfo::human_bytes(g_sampler.net_rx_rate()) + "/s   ↑ " +
                      procinfo::human_bytes(g_sampler.net_tx_rate()) + "/s";
  g_disk->push(0, g_sampler.disk_read_rate());
  g_disk->push(1, g_sampler.disk_write_rate());
  g_disk->value_text = "read " + procinfo::human_bytes(g_sampler.disk_read_rate()) + "/s   write " +
                       procinfo::human_bytes(g_sampler.disk_write_rate()) + "/s";
  for (auto *g : {g_cpu, g_mem, g_net, g_disk}) g->redraw();
  update_status();
}

static void tick(void *) {
  refresh();
  Fl::repeat_timeout(S.interval, tick);
}

static void restart_timer() {
  Fl::remove_timeout(tick);
  Fl::add_timeout(S.interval, tick);
}

static void signal_selected(int sig, bool confirm) {
  const Process *p = g_table->selected_process();
  if (!p) return;
  std::string name = fld::escape_at(p->name);
  pid_t pid = p->pid;
  if (confirm) {
    const char *what = sig == SIGKILL ? "Kill" : "End";
    const char *msg = sig == SIGKILL
                          ? "Kill \"%s\" (PID %d)?\nThe process is stopped immediately; unsaved data is lost."
                          : "End \"%s\" (PID %d)?\nThe process is asked to quit.";
    if (fl_choice(msg, "Cancel", what, nullptr, name.c_str(), (int)pid) != 1) return;
    if (pid == 1 && fl_choice("PID 1 is the init process. Ending it shuts down the whole system (or container).\n"
                              "Really continue?", "Cancel", what, nullptr) != 1)
      return;
  }
  int err = procinfo::send_signal(pid, sig);
  if (err == EPERM) {
    // someone else's process: ask polkit via pkexec
    if (!fld::which("pkexec").empty() &&
        fl_choice("You are not allowed to signal \"%s\".\nTry again as administrator?", "Cancel", "Authenticate",
                  nullptr, name.c_str()) == 1) {
      std::string out;
      fld::run_capture({"pkexec", "kill", "-" + std::to_string(sig), std::to_string(pid)}, out);
    }
  } else if (err) {
    fl_alert("Could not signal \"%s\": %s", name.c_str(), strerror(err));
  }
  refresh();
}

static void change_priority() {
  const Process *p = g_table->selected_process();
  if (!p) return;
  pid_t pid = p->pid;
  std::string cur = std::to_string(p->nice), name = fld::escape_at(p->name);
  const char *v = fl_input("Nice value for \"%s\" (-20 = highest priority, 19 = lowest):", cur.c_str(), name.c_str());
  if (!v) return;
  int n = std::max(-20, std::min(19, atoi(v)));
  int err = procinfo::set_nice(pid, n);
  if (err == EPERM || err == EACCES) {
    if (!fld::which("pkexec").empty() &&
        fl_choice("Raising the priority needs administrator rights.\nAuthenticate?", "Cancel", "Authenticate",
                  nullptr) == 1) {
      std::string out;
      fld::run_capture({"pkexec", "renice", "-n", std::to_string(n), "-p", std::to_string(pid)}, out);
    }
  } else if (err) {
    fl_alert("Could not change the priority: %s", strerror(err));
  }
  refresh();
}

static void show_properties() {
  const Process *p = g_table->selected_process();
  if (!p) return;
  std::string t;
  t += "Name: " + p->name + "\n";
  t += "PID: " + std::to_string(p->pid) + "    Parent: " + std::to_string(p->ppid) + "\n";
  t += "User: " + p->user + " (" + std::to_string(p->uid) + ")\n";
  t += "State: " + procinfo::state_name(p->state) + "    Threads: " + std::to_string(p->threads) + "\n";
  t += "Nice: " + std::to_string(p->nice) + "    CPU: " + fmt("%.1f%%", p->cpu) + "\n";
  t += "Memory (resident): " + procinfo::human_bytes((double)p->rss) + "\n";
  t += "Virtual memory: " + procinfo::human_bytes((double)p->vsize) + "\n";
  t += "Disk read: " + procinfo::human_bytes((double)p->read_bytes) + "    written: " +
       procinfo::human_bytes((double)p->write_bytes) + "\n";
  double started = (double)p->start_ticks / (double)g_sampler.clock_ticks();
  long age = (long)(g_sampler.system().uptime - started);
  t += "Running for: " + std::to_string(age / 3600) + "h " + std::to_string((age / 60) % 60) + "m " +
       std::to_string(age % 60) + "s\n";
  if (!p->exe.empty()) t += "Executable: " + p->exe + "\n";
  if (!p->cmdline.empty()) t += "Command: " + p->cmdline + "\n";
  fl_message_title("Process Properties");
  fl_message("%s", fld::escape_at(t).c_str());
}

static void process_menu(pid_t) {
  const Process *p = g_table->selected_process();
  if (!p) return;
  bool stopped = p->state == 'T';
  Fl_Menu_Item m[] = {
      {"End Process", FL_Delete, nullptr, (void *)1},
      {"Kill Process", FL_SHIFT + FL_Delete, nullptr, (void *)2, FL_MENU_DIVIDER},
      {"Stop", 0, nullptr, (void *)3, stopped ? FL_MENU_INACTIVE : 0},
      {"Continue", 0, nullptr, (void *)4, (stopped ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER},
      {"Change Priority…", 0, nullptr, (void *)5},
      {"Open File Location", 0, nullptr, (void *)6, (p->exe.empty() ? FL_MENU_INACTIVE : 0) | FL_MENU_DIVIDER},
      {"Properties", FL_ALT + FL_Enter, nullptr, (void *)7},
      {nullptr}};
  std::string exe = p->exe;
  const Fl_Menu_Item *r = m->popup(Fl::event_x(), Fl::event_y());
  if (!r) return;
  switch ((long)r->user_data()) {
    case 1: signal_selected(SIGTERM, true); break;
    case 2: signal_selected(SIGKILL, true); break;
    case 3: signal_selected(SIGSTOP, false); break;
    case 4: signal_selected(SIGCONT, false); break;
    case 5: change_priority(); break;
    case 6: fld::spawn({"fl-files", fld::dirname(exe)}); break;
    case 7: show_properties(); break;
  }
}

static void main_menu(Fl_Widget *w, void *) {
  Fl_Menu_Item m[] = {
      {"Update Speed", 0, nullptr, nullptr, FL_SUBMENU},
        {"Fast (0.5 s)", 0, nullptr, (void *)10, FL_MENU_RADIO | (S.interval == 0.5 ? FL_MENU_VALUE : 0)},
        {"Normal (1 s)", 0, nullptr, (void *)11, FL_MENU_RADIO | (S.interval == 1 ? FL_MENU_VALUE : 0)},
        {"Slow (2 s)", 0, nullptr, (void *)12, FL_MENU_RADIO | (S.interval == 2 ? FL_MENU_VALUE : 0)},
        {"Very slow (5 s)", 0, nullptr, (void *)13, FL_MENU_RADIO | (S.interval == 5 ? FL_MENU_VALUE : 0)},
        {nullptr},
      {"Show Kernel Threads", 0, nullptr, (void *)1, FL_MENU_TOGGLE | (S.kernel ? FL_MENU_VALUE : 0)},
      {"Graph Each CPU", 0, nullptr, (void *)2, FL_MENU_TOGGLE | (g_per_core ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER},
      {"Refresh Now", FL_F + 5, nullptr, (void *)3, FL_MENU_DIVIDER},
      {"About", 0, nullptr, (void *)4},
      {"Quit", FL_CTRL + 'q', nullptr, (void *)5},
      {nullptr}};
  const Fl_Menu_Item *r = m->popup(w->x(), w->y() + w->h());
  if (!r) return;
  long id = (long)r->user_data();
  switch (id) {
    case 10: case 11: case 12: case 13:
      S.interval = id == 10 ? 0.5 : id == 11 ? 1 : id == 12 ? 2 : 5;
      save_settings();
      restart_timer();
      break;
    case 1: S.kernel = !S.kernel; save_settings(); g_table->rebuild(); update_status(); break;
    case 2: g_per_core = !g_per_core; setup_cpu_series(); refresh(); break;
    case 3: refresh(); break;
    case 4: fld::about_dialog("Task Manager", "fl-taskmgr", "Shows running processes and system load."); break;
    case 5: g_win->hide(); break;
  }
}

int FilterInput::handle(int e) {
  if (e == FL_KEYBOARD) {
    int k = Fl::event_key();
    if (k == FL_Down || k == FL_Enter || k == FL_KP_Enter) {
      g_table->take_focus();
      if (!g_table->selected_process() && !g_table->items.empty()) {
        g_table->selected = g_table->items[0].p.pid;
        g_table->rows_changed();
        update_status();
      }
      return 1;
    }
  }
  return Fl_Input::handle(e);
}

static int global_keys(int e) {
  if (e != FL_SHORTCUT) return 0;
  int k = Fl::event_key(), st = Fl::event_state();
  if (k == FL_F + 5) { refresh(); return 1; }
  if ((st & FL_CTRL) && k == 'f') {
    g_filter->take_focus();
    g_filter->insert_position(0, g_filter->size());
    return 1;
  }
  if (k == FL_Escape && Fl::focus() == g_filter) {
    g_filter->value("");
    g_filter->do_callback();
    g_table->take_focus();
    return 1;
  }
  if ((st & FL_CTRL) && k == 'q') { g_win->hide(); return 1; }
  if (k == FL_Delete && Fl::focus() == g_table) {
    signal_selected((st & FL_SHIFT) ? SIGKILL : SIGTERM, true);
    return 1;
  }
  if ((st & FL_ALT) && (k == FL_Enter || k == FL_KP_Enter)) { show_properties(); return 1; }
  return 0;
}

int main(int argc, char **argv) {
  fld::init_app("fl-taskmgr", argc, argv);
  load_settings();
  bool perf = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-taskmgr [--performance]\n");
      return 0;
    }
    if (a == "-p" || a == "--performance") perf = true;
  }
  const fld::Palette &pal = fld::palette();
  const int W = 900, H = 600, TB = 42, ST = 24;
  g_win = new Fl_Double_Window(W, H, "Task Manager");

  Fl_Tabs *tabs = new Fl_Tabs(0, 0, W, H - ST);
  // ---- processes
  Fl_Group *pg = new Fl_Group(0, 30, W, H - ST - 30, "Processes");
  fld::Toolbar *bar = new fld::Toolbar(0, 30, W, TB);
  bar->color(pal.bg);
  Fl_Input *filter = g_filter = new FilterInput(8, 37, 240, 28);
  filter->tooltip("Filter by name, command, PID or user (Ctrl+F)");
  filter->when(FL_WHEN_CHANGED);
  filter->callback([](Fl_Widget *w, void *) {
    g_table->filter = fld::trim(((Fl_Input *)w)->value());
    g_table->rebuild();
    update_status();
  });
  bar->add_widget(filter);
  Fl_Check_Button *all = new Fl_Check_Button(260, 37, 150, 28, "All users");
  all->value(S.all_users);
  all->callback([](Fl_Widget *w, void *) {
    S.all_users = ((Fl_Check_Button *)w)->value();
    save_settings();
    g_table->rebuild();
    update_status();
  });
  bar->add_widget(all);
  Fl_Check_Button *tree = new Fl_Check_Button(410, 37, 140, 28, "Tree view");
  tree->value(S.tree);
  tree->tooltip("Show child processes under their parents (while not filtering)");
  tree->callback([](Fl_Widget *w, void *) {
    S.tree = ((Fl_Check_Button *)w)->value();
    save_settings();
    g_table->rebuild();
  });
  bar->add_widget(tree);
  Fl_Box *spacer = new Fl_Box(560, 30, 10, TB);
  bar->add(spacer);
  bar->resizable(spacer);
  g_end_btn = new Fl_Button(W - 160, 37, 110, 28, "End Process");
  g_end_btn->tooltip("Ask the selected process to quit (Delete; Shift+Delete kills it)");
  g_end_btn->callback([](Fl_Widget *, void *) { signal_selected(SIGTERM, true); });
  bar->add(g_end_btn);
  fld::FlatButton *menu = new fld::FlatButton(W - 42, 33, 36, 36);
  menu->color(pal.bg);
  menu->icon_name("open-menu", 20);
  if (!fld::icon("open-menu", 20)) menu->icon_name("preferences-system", 20);
  menu->tooltip("Menu");
  menu->callback(main_menu);
  bar->add(menu);
  bar->end();
  g_table = new ProcTable(0, 30 + TB, W, H - ST - 30 - TB);
  g_table->on_menu = process_menu;
  g_table->on_select = update_status;
  pg->resizable(g_table);
  pg->end();

  // ---- performance
  Fl_Group *perf_g = new Fl_Group(0, 30, W, H - ST - 30, "Performance");
  int gw = (W - 24) / 2, gh = (H - ST - 30 - 24) / 2;
  g_cpu = new HistoryGraph(8, 38, gw, gh, "CPU");
  g_cpu->fixed_max = 100;
  g_mem = new HistoryGraph(16 + gw, 38, gw, gh, "Memory");
  g_mem->fixed_max = 100;
  g_mem->add_series("Memory", series_color(3));
  g_mem->add_series("Swap", series_color(1));
  g_net = new HistoryGraph(8, 46 + gh, gw, gh, "Network");
  g_net->bytes = true;
  g_net->add_series("Received", series_color(0));
  g_net->add_series("Sent", series_color(2));
  g_disk = new HistoryGraph(16 + gw, 46 + gh, gw, gh, "Disk");
  g_disk->bytes = true;
  g_disk->add_series("Read", series_color(4));
  g_disk->add_series("Write", series_color(5));
  perf_g->end();
  tabs->end();
  tabs->resizable(pg);
  if (perf) tabs->value(perf_g);

  g_status = new Fl_Box(0, H - ST, W, ST);
  g_status->box(FL_FLAT_BOX);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(pal.fg_dim);
  g_win->resizable(tabs);
  g_win->end();
  g_win->size_range(560, 360);
  fld::set_window_icon(g_win, "fl-taskmgr");
  Fl::add_handler(global_keys);

  g_sampler.sample();
  setup_cpu_series();
  refresh();
  restart_timer();
  g_win->show();
  g_table->take_focus();
  return Fl::run();
}
