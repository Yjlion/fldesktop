// fl-logs: the system's logs. The systemd journal (through journalctl) or,
// without systemd, the syslog and OpenRC files in /var/log; filters by
// priority, program and text; follows new entries; colours by priority.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include "fld/crash.h"
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <set>
#include <string>
#include <vector>

#include "fld/menus.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "logs_util.h"

namespace {

const size_t kMaxEntries = 10000;

Fl_Double_Window *g_win;
Fl_Choice *g_source, *g_since, *g_prio, *g_program;
Fl_Input *g_search;
Fl_Check_Button *g_follow;
Fl_Box *g_status;
Fl_Text_Buffer *g_detail_buf;
class LogList;
LogList *g_list;

std::vector<std::string> g_sources; // "journal", "journal-user", or a file path
std::vector<logs::Entry> g_entries;
std::vector<int> g_shown; // indices into g_entries
logs::Filter g_filter;
bool g_have_journal = false;

// follow: journalctl -f, or the end of a file
pid_t g_follow_pid = -1;
int g_follow_fd = -1;
std::string g_follow_buf;
long long g_file_pos = 0;

Fl_Color prio_color(int p) {
  const fld::Palette &pal = fld::palette();
  if (p <= logs::CRIT) return pal.dark ? fl_rgb_color(0xff, 0x6b, 0x6b) : fl_rgb_color(0xb0, 0x00, 0x20);
  if (p == logs::ERR) return pal.dark ? fl_rgb_color(0xff, 0x8a, 0x80) : fl_rgb_color(0xc6, 0x28, 0x28);
  if (p == logs::WARNING) return pal.dark ? fl_rgb_color(0xff, 0xc1, 0x07) : fl_rgb_color(0xa0, 0x60, 0x00);
  if (p == logs::NOTICE) return pal.fg;
  if (p == logs::DEBUG) return pal.fg_dim;
  return pal.fg;
}

// The entries as a list with columns (time, priority, program, message),
// drawn by hand: fast for tens of thousands of rows.
class LogList : public Fl_Hold_Browser {
public:
  LogList(int x, int y, int w, int h) : Fl_Hold_Browser(x, y, w, h) {
    format_char(0);
    textsize(13);
    box(FL_FLAT_BOX);
    color(fld::palette().bg2);
  }
  void item_draw(void *item, int X, int Y, int W, int H) const override {
    int line = lineno(item);
    if (line < 1 || line > (int)g_shown.size()) return;
    const logs::Entry &e = g_entries[(size_t)g_shown[(size_t)line - 1]];
    bool sel = item_selected(item);
    Fl_Color fg = sel ? fl_contrast(fld::palette().fg, selection_color()) : prio_color(e.priority);
    fl_font(e.priority <= logs::ERR ? FL_HELVETICA_BOLD : FL_HELVETICA, textsize());
    fl_color(sel ? fg : fld::palette().fg_dim);
    std::string t = logs::format_time(e.time, time(nullptr));
    fl_draw(t.c_str(), X + 2, Y, 120, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    fl_color(fg);
    fl_draw(logs::priority_name(e.priority), X + 126, Y, 70, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    std::string src = e.source.empty() ? e.unit : e.source;
    fl_draw(src.c_str(), X + 200, Y, 140, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    std::string msg = e.message;
    std::replace(msg.begin(), msg.end(), '\n', ' ');
    fl_draw(msg.c_str(), X + 344, Y, W - 346, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  }
  int item_height(void *) const override { return textsize() + 6; }
  int item_width(void *) const override { return w() - 20; }
};

void status(const std::string &s) {
  static std::string keep;
  keep = s;
  g_status->label(keep.c_str());
  g_status->redraw();
}

void refilter(bool keep_selection = false) {
  int sel = keep_selection && g_list->value() > 0 ? g_shown[(size_t)g_list->value() - 1] : -1;
  bool at_end = g_list->size() == 0 || g_list->displayed(g_list->size());
  g_filter.text = g_search->value();
  static const int map[] = {7, 6, 5, 4, 3, 2};
  g_filter.max_priority = map[std::max(0, std::min(5, g_prio->value()))];
  g_filter.source = g_program->value() > 0 && g_program->text() ? g_program->text() : "";
  g_shown.clear();
  for (size_t i = 0; i < g_entries.size(); i++)
    if (logs::matches(g_entries[i], g_filter)) g_shown.push_back((int)i);
  g_list->clear();
  for (size_t i = 0; i < g_shown.size(); i++) g_list->add("");
  if (sel >= 0) {
    auto it = std::find(g_shown.begin(), g_shown.end(), sel);
    if (it != g_shown.end()) g_list->select((int)(it - g_shown.begin()) + 1);
  }
  // the newest at the bottom stay in view
  if (at_end && !g_shown.empty()) g_list->bottomline(g_list->size());
  status(std::to_string(g_shown.size()) + " of " + std::to_string(g_entries.size()) + " entries");
}

void fill_programs() {
  std::string cur = g_program->value() > 0 && g_program->text() ? g_program->text() : "";
  std::set<std::string> names;
  for (const logs::Entry &e : g_entries)
    if (!e.source.empty()) names.insert(e.source);
  g_program->clear();
  g_program->add("All programs");
  int pick = 0, i = 1;
  for (const std::string &n : names) {
    g_program->add(fld::Menu::text(n).c_str());
    if (n == cur) pick = i;
    i++;
  }
  g_program->value(pick);
}

void add_entry(logs::Entry &&e) {
  g_entries.push_back(std::move(e));
  if (g_entries.size() > kMaxEntries) g_entries.erase(g_entries.begin(), g_entries.begin() + (long)(kMaxEntries / 10));
}

int current_year() {
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  return 1900 + tm.tm_year;
}

int g_file_fd = -1;

void file_poll(void *) {
  if (g_file_fd < 0) return;
  char buf[65536];
  ssize_t n;
  bool any = false;
  while ((n = read(g_file_fd, buf, sizeof buf)) > 0) {
    g_follow_buf.append(buf, (size_t)n);
    any = true;
  }
  if (any) {
    size_t last = g_follow_buf.rfind('\n');
    if (last != std::string::npos) {
      std::string complete = g_follow_buf.substr(0, last);
      g_follow_buf.erase(0, last + 1);
      int year = current_year();
      for (const std::string &line : fld::split(complete, '\n')) {
        if (line.empty()) continue;
        add_entry(logs::parse_syslog_line(line, year));
      }
      refilter(true);
    }
  }
  Fl::repeat_timeout(0.5, file_poll);
}

void stop_follow() {
  if (g_follow_fd >= 0) {
    Fl::remove_fd(g_follow_fd);
    close(g_follow_fd);
    g_follow_fd = -1;
  }
  if (g_follow_pid > 0) {
    kill(g_follow_pid, SIGTERM);
    waitpid(g_follow_pid, nullptr, 0);
    g_follow_pid = -1;
  }
  Fl::remove_timeout(file_poll);
  if (g_file_fd >= 0) {
    close(g_file_fd);
    g_file_fd = -1;
  }
  g_follow_buf.clear();
}


bool is_journal() { return g_source->value() >= 0 && fld::starts_with(g_sources[(size_t)g_source->value()], "journal"); }

std::vector<std::string> journal_args(bool follow) {
  std::vector<std::string> a = {"journalctl", "-o", "json", "--no-pager", "-q"};
  if (g_sources[(size_t)g_source->value()] == "journal-user") a.push_back("--user");
  if (follow) {
    a.push_back("-f");
    a.push_back("-n");
    a.push_back("0");
    return a;
  }
  switch (g_since->value()) {
    case 0: a.push_back("-b"); break;              // this boot
    case 1: a.push_back("-b"); a.push_back("-1"); break; // the boot before
    case 2: a.push_back("--since=-1h"); break;
    case 3: a.push_back("--since=today"); break;
    case 4: a.push_back("--since=-7d"); break;
  }
  a.push_back("-n");
  a.push_back(std::to_string(kMaxEntries));
  return a;
}

void on_follow_data(int fd, void *) {
  char buf[65536];
  ssize_t n = read(fd, buf, sizeof buf);
  if (n <= 0) {
    stop_follow();
    return;
  }
  g_follow_buf.append(buf, (size_t)n);
  size_t nl;
  bool any = false;
  int year = 1900;
  {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    year += tm.tm_year;
  }
  while ((nl = g_follow_buf.find('\n')) != std::string::npos) {
    std::string line = g_follow_buf.substr(0, nl);
    g_follow_buf.erase(0, nl + 1);
    if (line.empty()) continue;
    logs::Entry e;
    if (is_journal()) {
      if (!logs::parse_journal_json(line, e)) continue;
    } else {
      e = logs::parse_syslog_line(line, year);
      e.fields["FILE"] = g_sources[(size_t)g_source->value()];
    }
    add_entry(std::move(e));
    any = true;
  }
  if (any) refilter(true);
}

void start_follow() {
  stop_follow();
  if (!g_follow->value()) return;
  if (is_journal()) {
    int p[2];
    if (pipe2(p, O_CLOEXEC) != 0) return;
    std::vector<std::string> args = journal_args(true);
    pid_t pid = fork();
    if (pid == 0) {
      dup2(p[1], 1);
      int null = open("/dev/null", O_WRONLY);
      if (null >= 0) dup2(null, 2);
      std::vector<char *> av;
      for (auto &s : args) av.push_back((char *)s.c_str());
      av.push_back(nullptr);
      execvp(av[0], av.data());
      _exit(127);
    }
    close(p[1]);
    if (pid < 0) {
      close(p[0]);
      return;
    }
    g_follow_pid = pid;
    g_follow_fd = p[0];
    Fl::add_fd(g_follow_fd, FL_READ, on_follow_data);
  } else {
    // a file: read what was appended, twice a second
    std::string path = g_sources[(size_t)g_source->value()];
    g_file_fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (g_file_fd < 0) return;
    lseek(g_file_fd, g_file_pos, SEEK_SET);
    Fl::add_timeout(0.5, file_poll);
  }
}

void load() {
  stop_follow();
  g_entries.clear();
  fl_cursor(FL_CURSOR_WAIT);
  Fl::flush();
  std::string src = g_sources[(size_t)g_source->value()];
  int year = 1900;
  {
    time_t now = time(nullptr);
    struct tm tm;
    localtime_r(&now, &tm);
    year += tm.tm_year;
  }
  if (is_journal()) {
    g_since->activate();
    fld::run_lines(journal_args(false), [](const std::string &line) {
      logs::Entry e;
      if (logs::parse_journal_json(line, e)) add_entry(std::move(e));
    });
    if (g_entries.empty() && getuid() != 0)
      status("No entries. To read the system journal you may need to be in the \"systemd-journal\" or \"adm\" group.");
  } else {
    g_since->deactivate();
    std::string text;
    if (!fld::read_file(src, text, 64u << 20)) {
      fl_cursor(FL_CURSOR_DEFAULT);
      refilter();
      status("Cannot read " + src + " (you may need to be in the \"adm\" or \"wheel\" group).");
      return;
    }
    // the last entries of a big file
    size_t start = text.size() > (16u << 20) ? text.find('\n', text.size() - (16u << 20)) + 1 : 0;
    for (const std::string &line : fld::split(text.substr(start), '\n')) {
      if (line.empty()) continue;
      logs::Entry e = logs::parse_syslog_line(line, year);
      e.fields["FILE"] = src;
      add_entry(std::move(e));
    }
    g_file_pos = (long long)text.size();
  }
  fl_cursor(FL_CURSOR_DEFAULT);
  fill_programs();
  refilter();
  if (!g_shown.empty()) g_list->bottomline(g_list->size());
  start_follow();
}

void show_detail() {
  int i = g_list->value() - 1;
  if (i < 0 || i >= (int)g_shown.size()) {
    g_detail_buf->text("");
    return;
  }
  const logs::Entry &e = g_entries[(size_t)g_shown[(size_t)i]];
  std::string t;
  char when[64] = "";
  time_t tt = (time_t)e.time;
  if (e.time > 0) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&tt));
  t += std::string(when) + "  " + logs::priority_name(e.priority) + "  " + e.source;
  if (e.pid) t += "[" + std::to_string(e.pid) + "]";
  if (!e.unit.empty() && e.unit != e.source) t += "  (" + e.unit + ")";
  t += "\n\n" + e.message + "\n";
  if (!e.fields.empty()) {
    t += "\n";
    for (auto &f : e.fields)
      if (f.first != "MESSAGE" && f.first.compare(0, 2, "__") != 0) t += f.first + "=" + f.second + "\n";
  }
  g_detail_buf->text(t.c_str());
}

void export_shown() {
  Fl_Native_File_Chooser fc;
  fc.title("Save the Shown Entries");
  fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  fc.preset_file("log.txt");
  fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
  if (fc.show() != 0) return;
  std::string out;
  for (int i : g_shown) {
    const logs::Entry &e = g_entries[(size_t)i];
    char when[64] = "";
    time_t tt = (time_t)e.time;
    if (e.time > 0) strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&tt));
    out += std::string(when) + " " + logs::priority_name(e.priority) + " " + e.source + ": " + e.message + "\n";
  }
  if (!fld::write_file(fc.filename(), out)) fl_alert("Cannot write %s", fc.filename());
  else status("Saved " + std::to_string(g_shown.size()) + " entries to " + fld::tilde(fc.filename()));
}

} // namespace

// ---- crash reports ---------------------------------------------------------

// fldesktop's own reports (fld::install_crash_handler) and, with systemd,
// coredumpctl's list of crashed fl-* programs
void crashes_dialog() {
  static std::vector<fld::CrashReport> reports;
  reports = fld::crash_reports();
  std::string cores;
  if (!fld::which("coredumpctl").empty()) {
    std::string out;
    if (fld::run_capture({"coredumpctl", "list", "--no-pager", "--no-legend", "-r", "-q"}, out) == 0)
      for (auto &l : fld::split(out, '\n'))
        if (l.find(" fl-") != std::string::npos || l.find("/fl-") != std::string::npos ||
            l.find("xdg-desktop-portal-fl") != std::string::npos)
          cores += l + "\n";
  }
  Fl_Double_Window d(640, 460, "Crash Reports");
  d.set_modal();
  static Fl_Hold_Browser *list;
  static Fl_Text_Buffer *buf;
  list = new Fl_Hold_Browser(10, 10, 620, 150);
  buf = new Fl_Text_Buffer();
  Fl_Text_Display *view = new Fl_Text_Display(10, 168, 620, 240);
  view->buffer(buf);
  view->textfont(FL_COURIER);
  view->textsize(12);
  for (auto &r : reports) {
    char when[32];
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&r.when));
    list->add((std::string(when) + "   " + fld::escape_at(r.program) + "   " + r.signal).c_str());
  }
  if (reports.empty()) list->add("@iNo crash reports from fldesktop programs");
  if (!cores.empty()) buf->text(("Core dumps of fldesktop programs (coredumpctl):\n" + cores).c_str());
  list->callback([](Fl_Widget *, void *) {
    int i = list->value() - 1;
    if (i >= 0 && i < (int)reports.size()) buf->text(reports[i].text.c_str());
  });
  Fl_Button *report = new Fl_Button(10, 420, 160, 30, "Report on GitHub…");
  report->tooltip("Open a new issue with this report filled in (in the web browser); look at it before you send it");
  report->callback([](Fl_Widget *, void *) {
    int i = list->value() - 1;
    if (i < 0 || i >= (int)reports.size()) return;
    fld::open_path(fld::crash_issue_url(reports[i]));
  });
  Fl_Button *del = new Fl_Button(176, 420, 100, 30, "Delete");
  del->callback([](Fl_Widget *w, void *) {
    int i = list->value() - 1;
    if (i < 0 || i >= (int)reports.size()) return;
    unlink(reports[i].path.c_str());
    reports.erase(reports.begin() + i);
    list->remove(i + 1);
    buf->text("");
  });
  Fl_Button *close = new Fl_Button(530, 420, 100, 30, "Close");
  close->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  d.end();
  d.show();
  while (d.shown()) Fl::wait();
}

int main(int argc, char **argv) {
  fld::init_app("fl-logs", argc, argv);
  std::string want_file;
  bool want_user = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-logs [--user | FILE]\n");
      return 0;
    }
    if (a == "--user") want_user = true;
    else if (a[0] != '-') want_file = fld::abs_path(a);
  }
  g_have_journal = !fld::which("journalctl").empty() && fld::is_dir("/run/systemd/system");
  if (g_have_journal) {
    g_sources.push_back("journal");
    g_sources.push_back("journal-user");
  }
  for (const std::string &f : logs::log_files("/var/log")) g_sources.push_back(f);
  for (const std::string &f : logs::log_files("/var/log/rc")) g_sources.push_back(f);
  if (!want_file.empty() && std::find(g_sources.begin(), g_sources.end(), want_file) == g_sources.end())
    g_sources.push_back(want_file);
  if (g_sources.empty()) g_sources.push_back("/var/log/messages");

  const int W = 1100, H = 680;
  g_win = new Fl_Double_Window(W, H, "Logs");
  int y = 8;
  g_source = new Fl_Choice(64, y, 240, 28, "Log:");
  for (const std::string &s : g_sources) {
    std::string label = s == "journal" ? "System journal" : s == "journal-user" ? "My journal" : fld::tilde(s);
    g_source->add(fld::Menu::text(label).c_str());
  }
  int pick = 0;
  for (size_t i = 0; i < g_sources.size(); i++) {
    if (want_user && g_sources[i] == "journal-user") pick = (int)i;
    if (!want_file.empty() && g_sources[i] == want_file) pick = (int)i;
  }
  g_source->value(pick);
  g_source->callback([](Fl_Widget *, void *) { load(); });
  g_since = new Fl_Choice(360, y, 150, 28, "From:");
  g_since->add("This boot|The boot before|The last hour|Today|The last 7 days");
  g_since->value(0);
  g_since->callback([](Fl_Widget *, void *) { load(); });
  g_prio = new Fl_Choice(580, y, 130, 28, "Show:");
  g_prio->add("Everything|Info and up|Notices and up|Warnings and up|Errors and up|Critical only");
  // index -> max priority: 7, 6, 5, 4, 3, 2
  g_prio->value(1);
  g_prio->callback([](Fl_Widget *, void *) { refilter(true); });
  g_follow = new fld::CheckButton(726, y, 90, 28, "Follow");
  g_follow->value(1);
  g_follow->tooltip("Show new entries as they come");
  g_follow->callback([](Fl_Widget *, void *) { start_follow(); });
  Fl_Button *crashes = new Fl_Button(W - 292, y, 90, 28, "Crashes…");
  crashes->tooltip("Crash reports of the fldesktop programs, to look at or report");
  crashes->callback([](Fl_Widget *, void *) { crashes_dialog(); });
  Fl_Button *save = new Fl_Button(W - 196, y, 90, 28, "Save…");
  save->tooltip("Save the shown entries as text");
  save->callback([](Fl_Widget *, void *) { export_shown(); });
  Fl_Button *reload = new Fl_Button(W - 100, y, 92, 28, "Reload");
  reload->callback([](Fl_Widget *, void *) { load(); });
  y += 36;
  g_program = new Fl_Choice(64, y, 240, 28, "From:");
  g_program->callback([](Fl_Widget *, void *) { refilter(); });
  g_search = new Fl_Input(360, y, W - 368, 28, "Find:");
  g_search->when(FL_WHEN_CHANGED);
  g_search->callback([](Fl_Widget *, void *) { refilter(); });
  y += 36;
  Fl_Tile *tile = new Fl_Tile(0, y, W, H - y - 26);
  g_list = new LogList(0, y, W, (H - y - 26) * 2 / 3);
  g_list->callback([](Fl_Widget *, void *) { show_detail(); });
  g_detail_buf = new Fl_Text_Buffer();
  Fl_Text_Display *detail = new Fl_Text_Display(0, y + (H - y - 26) * 2 / 3, W, (H - y - 26) - (H - y - 26) * 2 / 3);
  detail->buffer(g_detail_buf);
  detail->textfont(FL_COURIER);
  detail->textsize(12);
  detail->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
  tile->end();
  g_status = new Fl_Box(8, H - 26, W - 16, 24);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->end();
  g_win->resizable(tile);
  g_win->size_range(700, 400);
  g_win->callback([](Fl_Widget *w, void *) {
    stop_follow();
    w->hide();
  });
  fld::set_window_icon(g_win, "fl-logs");
  // the filter for the default choice ("Info and up")
  g_filter.max_priority = 6;
  g_win->show();
  status("Reading the log…");
  // read once the window is up
  Fl::add_timeout(0.1, [](void *) { load(); });
  int r = Fl::run();
  stop_follow();
  return r;
}
