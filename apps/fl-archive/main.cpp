// fl-archive: archive manager for fldesktop (libarchive). Browses, extracts,
// creates and changes zip, 7z, tar (gz, xz, zstd, bz2) archives and reads
// many more formats (rar, iso, cpio, deb, ...).
//
//   fl-archive [ARCHIVE]                 open an archive
//   fl-archive --extract-here FILE...    extract next to the archive
//   fl-archive --extract-to DIR FILE...  extract into DIR
//   fl-archive --extract-ask FILE...     ask where to extract
//   fl-archive --compress FILE...        ask for a name and format, create
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/Fl_Table_Row.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "archive_core.h"
#include "fld/file_dialog.h"
#include "fld/icons.h"
#include "fld/menus.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

// ---- background jobs with a progress window ------------------------------------------

struct Job {
  std::string title;
  std::function<arc::Result(const arc::Progress &, Job &)> work;
  std::function<void(const arc::Result &)> done;
  // shared with the worker
  std::atomic<bool> cancel{false}, finished{false};
  std::mutex mu;
  uint64_t pos = 0, total = 0;
  std::string current;
  arc::Result result;
  // a question for the user (file conflicts), answered in the FLTK thread
  std::mutex qmu;
  std::condition_variable qcv;
  std::string question;
  int answer = -1;
  bool asking = false; // the dialog is open (FLTK thread only)
};

static Fl_Double_Window *g_progress_win;
static bool g_batch = false; // command line mode: no main window
static Fl_Box *g_progress_title, *g_progress_file;
static Fl_Progress *g_progress;

static void build_progress_window() {
  if (g_progress_win) return;
  g_progress_win = new Fl_Double_Window(460, 140, "Working");
  g_progress_title = new Fl_Box(16, 12, 428, 24);
  g_progress_title->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_progress_title->labelfont(FL_HELVETICA_BOLD);
  g_progress_file = new Fl_Box(16, 38, 428, 20);
  g_progress_file->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_progress_file->labelsize(12);
  g_progress = new Fl_Progress(16, 64, 428, 20);
  g_progress->selection_color(fld::palette().accent);
  g_progress->minimum(0);
  g_progress->maximum(1);
  Fl_Button *cancel = new Fl_Button(344, 98, 100, 30, "Cancel");
  cancel->callback([](Fl_Widget *w, void *) {
    Job *j = (Job *)w->window()->user_data();
    if (j) j->cancel = true;
  });
  g_progress_win->end();
  g_progress_win->set_modal();
  g_progress_win->callback([](Fl_Widget *w, void *) {
    Job *j = (Job *)w->user_data();
    if (j) j->cancel = true;
  });
  fld::set_window_icon(g_progress_win, "fl-archive");
}

// asks the user about a file that exists already; from the FLTK thread
static arc::Conflict conflict_dialog(const std::string &path) {
  Fl_Double_Window win(480, 150, "File Exists");
  std::string msg = "\"" + fld::escape_at(path) + "\" exists already.";
  Fl_Box text(16, 12, 448, 60, nullptr);
  text.copy_label(msg.c_str());
  text.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP | FL_ALIGN_TOP);
  int result = arc::CANCEL;
  struct B {
    const char *label;
    arc::Conflict c;
  } buttons[] = {{"Skip All", arc::SKIP_ALL}, {"Skip", arc::SKIP}, {"Replace All", arc::REPLACE_ALL},
                 {"Replace", arc::REPLACE}};
  int x = 480 - 16 - 4 * 104 + 4;
  for (auto &b : buttons) {
    Fl_Button *btn = b.c == arc::REPLACE ? new Fl_Return_Button(x, 104, 100, 30, b.label)
                                         : new Fl_Button(x, 104, 100, 30, b.label);
    btn->callback([](Fl_Widget *w, void *d) {
      *(int *)w->window()->user_data() = (int)(long)d;
      w->window()->hide();
    }, (void *)(long)b.c);
    x += 104;
  }
  win.end();
  win.user_data(&result);
  win.callback([](Fl_Widget *w, void *) { w->hide(); });
  win.set_modal();
  win.show();
  while (win.shown()) Fl::wait();
  return (arc::Conflict)result;
}

static void job_awake(void *d) {
  Job *j = static_cast<Job *>(d);
  if (j->finished) return; // handled by the finish callback
  std::string q;
  {
    std::lock_guard<std::mutex> l(j->qmu);
    q = j->question;
  }
  if (!q.empty() && j->answer < 0 && !j->asking) {
    // other messages arrive while the dialog runs its own event loop
    j->asking = true;
    int a = conflict_dialog(q);
    j->asking = false;
    std::lock_guard<std::mutex> l(j->qmu);
    j->answer = a;
    j->question.clear();
    j->qcv.notify_all();
  }
  std::lock_guard<std::mutex> l(j->mu);
  g_progress->value(j->total ? (float)((double)j->pos / j->total) : 0.f);
  g_progress_file->copy_label(fld::escape_at(j->current).c_str());
}

static void job_finish(void *d) {
  Job *j = static_cast<Job *>(d);
  if (!g_batch) g_progress_win->hide();
  g_progress_win->user_data(nullptr);
  if (j->done) j->done(j->result);
  delete j;
}

// Runs the job in a thread; the progress window shows after a short delay.
static void run_job(Job *j) {
  build_progress_window();
  g_progress_win->user_data(j);
  g_progress_win->copy_label(j->title.c_str());
  g_progress_title->copy_label(fld::escape_at(j->title).c_str());
  g_progress_file->label("");
  g_progress->value(0);
  if (g_batch) g_progress_win->show();
  else
    Fl::add_timeout(0.4, [](void *) {
      if (g_progress_win->user_data()) g_progress_win->show();
    });
  std::thread([j] {
    arc::Progress prog = [j](uint64_t pos, uint64_t total, const std::string &cur) {
      {
        std::lock_guard<std::mutex> l(j->mu);
        j->pos = pos;
        j->total = total;
        j->current = cur;
      }
      Fl::awake(job_awake, j);
      return !j->cancel.load();
    };
    arc::Result r = j->work(prog, *j);
    if (j->cancel) r.cancelled = true;
    j->result = r;
    j->finished = true;
    Fl::awake(job_finish, j);
  }).detach();
}

// conflict callback for extract(): blocks the worker until the user answers
static std::function<arc::Conflict(const std::string &)> ask_from(Job &j) {
  return [&j](const std::string &path) {
    std::unique_lock<std::mutex> l(j.qmu);
    j.question = path;
    j.answer = -1;
    Fl::awake(job_awake, &j);
    j.qcv.wait(l, [&j] { return j.answer >= 0 || j.cancel.load(); });
    return j.cancel ? arc::CANCEL : (arc::Conflict)j.answer;
  };
}

static std::string ask_password(const std::string &archive, bool again) {
  std::string msg = (again ? "The password is not right. " : "") + std::string("\"") +
                    fld::escape_at(fld::basename(archive)) + "\" is protected by a password. Password:";
  const char *p = fl_password("%s", "", msg.c_str());
  return p ? p : "";
}

static void show_error(const std::string &what, const arc::Result &r) {
  if (r.cancelled) return;
  fl_alert("%s\n\n%s", fld::escape_at(what).c_str(), fld::escape_at(r.error).c_str());
}

static std::string unique_path(const std::string &p) {
  if (!fld::file_exists(p) && !fld::is_dir(p)) return p;
  for (int n = 2;; n++) {
    std::string c = p + " (" + std::to_string(n) + ")";
    if (!fld::file_exists(c) && !fld::is_dir(c)) return c;
  }
}

// ---- extracting -------------------------------------------------------------------------------

struct ExtractRequest {
  std::string archive, dest, password;
  std::vector<std::string> only;
  std::string strip;
  bool here = false;  // choose a folder next to the archive (see extract_here)
  bool open_after = false;
  std::function<void(bool ok, const std::string &where)> after;
};

static void start_extract(std::shared_ptr<ExtractRequest> req);

static void extract_here_dest(ExtractRequest &req, const std::vector<arc::Entry> &entries) {
  // one top-level folder (or a single file): put it next to the archive;
  // several items: into a new folder named after the archive
  std::string parent = fld::dirname(req.archive);
  std::string root = arc::common_root(entries);
  bool single = entries.size() == 1 && !entries[0].dir;
  if (!root.empty() || single) {
    req.dest = parent;
  } else {
    req.dest = unique_path(parent + "/" + arc::strip_archive_ext(fld::basename(req.archive)));
  }
}

static void start_extract(std::shared_ptr<ExtractRequest> req) {
  Job *j = new Job;
  j->title = "Extracting " + fld::basename(req->archive);
  j->work = [req](const arc::Progress &p, Job &job) {
    arc::ExtractOptions o;
    if (req->here) {
      std::vector<arc::Entry> entries;
      arc::Result lr = arc::list(req->archive, entries, req->password);
      if (!lr.ok) return lr;
      extract_here_dest(*req, entries);
    }
    o.dest = req->dest;
    o.only = req->only;
    o.strip = req->strip;
    o.password = req->password;
    o.progress = p;
    o.on_conflict = ask_from(job);
    return arc::extract(req->archive, o);
  };
  j->done = [req](const arc::Result &r) {
    if (r.need_password) {
      std::string pw = ask_password(req->archive, !req->password.empty());
      if (!pw.empty()) {
        req->password = pw;
        start_extract(req);
        return;
      }
    }
    if (!r.ok) show_error("Could not extract " + fld::basename(req->archive) + ".", r);
    else if (!r.skipped.empty())
      fl_message("%d item(s) were skipped (existing files, or unsafe paths such as \"../\").", (int)r.skipped.size());
    if (req->after) req->after(r.ok, req->dest);
  };
  run_job(j);
}

// ---- creating ---------------------------------------------------------------------------------

struct CreateRequest {
  std::string archive, password;
  std::vector<std::string> files;
  arc::Format format = arc::ZIP;
  std::function<void(bool ok)> after;
};

static void start_create(std::shared_ptr<CreateRequest> req) {
  Job *j = new Job;
  j->title = "Creating " + fld::basename(req->archive);
  j->work = [req](const arc::Progress &p, Job &) {
    arc::CreateOptions o;
    o.format = req->format;
    o.password = req->password;
    o.progress = p;
    return arc::create(req->archive, req->files, o);
  };
  j->done = [req](const arc::Result &r) {
    if (!r.ok) show_error("Could not create " + fld::basename(req->archive) + ".", r);
    if (req->after) req->after(r.ok);
  };
  run_job(j);
}

// The "Compress" dialog: name, format, folder, password. Calls `after` with
// the request when accepted (nullptr when cancelled).
static void compress_dialog(const std::vector<std::string> &files,
                            std::function<void(std::shared_ptr<CreateRequest>)> after) {
  if (files.empty()) return;
  std::string folder = fld::dirname(files[0]);
  std::string name = files.size() == 1 ? fld::basename(files[0]) : fld::basename(folder);
  if (name.empty() || name == "/") name = "Archive";
  Fl_Double_Window *win = new Fl_Double_Window(500, 210, "Compress");
  std::string what = files.size() == 1 ? "\"" + fld::basename(files[0]) + "\""
                                       : std::to_string(files.size()) + " items";
  Fl_Box *head = new Fl_Box(16, 10, 468, 26);
  head->copy_label(fld::escape_at("Compress " + what).c_str());
  head->labelfont(FL_HELVETICA_BOLD);
  head->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  Fl_Input *in = new Fl_Input(110, 46, 230, 30, "Name:");
  in->value(name.c_str());
  Fl_Choice *fmt = new Fl_Choice(346, 46, 138, 30);
  for (auto &f : arc::formats()) {
    std::string l = f.ext;
    fmt->add(l.c_str());
  }
  fmt->value(0);
  Fl_Box *where = new Fl_Box(110, 84, 270, 30);
  where->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  Fl_Box *where_label = new Fl_Box(16, 84, 90, 30, "Folder:");
  where_label->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  Fl_Button *change = new Fl_Button(384, 84, 100, 30, "Change…");
  Fl_Secret_Input *pw = new Fl_Secret_Input(110, 122, 230, 30, "Password:");
  pw->tooltip("Optional: encrypts the files (AES-256). Only zip archives can have a password.");
  Fl_Box *note = new Fl_Box(346, 122, 138, 30, "(zip only)");
  note->labelsize(12);
  note->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  Fl_Button *cancel = new Fl_Button(276, 166, 100, 30, "Cancel");
  Fl_Return_Button *ok = new Fl_Return_Button(384, 166, 100, 30, "Create");
  win->end();
  win->set_modal();
  fld::set_window_icon(win, "fl-archive");

  struct State {
    std::string folder;
    Fl_Box *where;
    Fl_Choice *fmt;
    Fl_Secret_Input *pw;
    int result = 0;
  };
  auto st = std::make_shared<State>();
  st->folder = folder;
  st->where = where;
  st->fmt = fmt;
  st->pw = pw;
  where->copy_label(fld::escape_at(folder).c_str());
  auto update_pw = [](State *s) {
    if (arc::formats()[(size_t)std::max(0, s->fmt->value())].password) s->pw->activate();
    else s->pw->deactivate();
  };
  update_pw(st.get());
  fmt->callback([](Fl_Widget *, void *d) {
    State *s = (State *)d;
    if (arc::formats()[(size_t)std::max(0, s->fmt->value())].password) s->pw->activate();
    else s->pw->deactivate();
  }, st.get());
  change->callback([](Fl_Widget *, void *d) {
    State *s = (State *)d;
    fld::FileDialogOptions o;
    o.title = "Choose a Folder";
    o.directory = true;
    o.folder = s->folder;
    o.accept_label = "Select";
    auto r = fld::run_file_dialog(o);
    if (r.ok && !r.paths.empty()) {
      s->folder = r.paths[0];
      s->where->copy_label(fld::escape_at(s->folder).c_str());
    }
  }, st.get());
  cancel->callback([](Fl_Widget *w, void *d) { ((State *)d)->result = 1; w->window()->hide(); }, st.get());
  ok->callback([](Fl_Widget *w, void *d) { ((State *)d)->result = 2; w->window()->hide(); }, st.get());
  win->callback([](Fl_Widget *w, void *d) { ((State *)d)->result = 1; w->hide(); }, st.get());
  win->show();
  in->take_focus();
  while (win->shown()) Fl::wait();
  if (st->result != 2 || !*in->value()) {
    Fl::delete_widget(win);
    after(nullptr);
    return;
  }
  const arc::FormatInfo &fi = arc::formats()[(size_t)std::max(0, fmt->value())];
  auto req = std::make_shared<CreateRequest>();
  std::string n = in->value();
  if (!fld::ends_with(fld::lower(n), fi.ext)) n += fi.ext;
  req->archive = st->folder + "/" + n;
  req->format = fi.format;
  req->files = files;
  if (fi.password) req->password = pw->value();
  Fl::delete_widget(win);
  if (fld::file_exists(req->archive) &&
      fl_choice("\"%s\" exists already. Replace it?", "Cancel", "Replace", nullptr,
                fld::escape_at(fld::basename(req->archive)).c_str()) != 1) {
    after(nullptr);
    return;
  }
  after(req);
}

// ---- the archive browser ------------------------------------------------------------------

struct Node {
  std::string name, path; // path in the archive
  bool dir = false, encrypted = false;
  int64_t size = 0;
  time_t mtime = 0;
  int items = 0; // folders: number of direct children
};

class ArchiveView;
static Fl_Double_Window *g_win;
static ArchiveView *g_view;
static Fl_Box *g_path_box, *g_status;
static fld::FlatButton *g_up;
static std::string g_archive, g_password, g_dir;
static std::vector<arc::Entry> g_entries;
static std::map<std::string, std::vector<Node>> g_tree; // folder -> children
static bool g_writable = false;
static fld::FlatButton *g_btn_extract, *g_btn_add, *g_btn_add_dir, *g_btn_delete;

static void open_archive(const std::string &path, const std::string &password = "");
static void show_dir(const std::string &dir);

static void build_tree() {
  g_tree.clear();
  std::map<std::string, Node> all;
  auto ensure_dir = [&](const std::string &p) {
    std::string cur;
    size_t start = 0;
    while (start < p.size()) {
      size_t e = p.find('/', start);
      cur = p.substr(0, e);
      if (!all.count(cur)) {
        Node n;
        n.path = cur;
        n.name = fld::basename(cur);
        n.dir = true;
        all[cur] = n;
      }
      if (e == std::string::npos) break;
      start = e + 1;
    }
  };
  for (auto &e : g_entries) {
    std::string parent = e.path.find('/') == std::string::npos ? "" : e.path.substr(0, e.path.rfind('/'));
    if (!parent.empty()) ensure_dir(parent);
    Node &n = all[e.path];
    n.path = e.path;
    n.name = fld::basename(e.path);
    n.dir = n.dir || e.dir;
    n.size = e.size;
    n.mtime = e.mtime;
    n.encrypted = e.encrypted;
  }
  for (auto &kv : all) {
    const Node &n = kv.second;
    std::string parent = n.path.find('/') == std::string::npos ? "" : n.path.substr(0, n.path.rfind('/'));
    g_tree[parent].push_back(n);
  }
  for (auto &kv : g_tree) {
    for (auto &n : kv.second)
      if (n.dir) n.items = g_tree.count(n.path) ? (int)g_tree[n.path].size() : 0;
    std::sort(kv.second.begin(), kv.second.end(), [](const Node &a, const Node &b) {
      if (a.dir != b.dir) return a.dir;
      return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
  }
  // folder item counts (children of folders inserted above)
  for (auto &kv : g_tree)
    for (auto &n : kv.second)
      if (n.dir) n.items = g_tree.count(n.path) ? (int)g_tree[n.path].size() : 0;
}

static std::string date_text(time_t t) {
  if (!t) return "";
  char b[32];
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
  return b;
}

static Fl_Image *node_icon(const Node &n) {
  if (n.dir) return fld::icon_first({"folder"}, 16);
  std::string mime = fld::mime_from_name(n.name);
  std::vector<std::string> names = fld::mime_icon_names(mime);
  names.push_back("text-x-generic");
  return fld::icon_first(names, 16);
}

class ArchiveView : public Fl_Table_Row {
public:
  std::vector<Node> rows_;
  ArchiveView(int x, int y, int w, int h) : Fl_Table_Row(x, y, w, h) {
    cols(4);
    col_header(1);
    col_header_height(26);
    col_resize(1);
    row_height_all(24);
    type(SELECT_MULTI);
    when(FL_WHEN_RELEASE | FL_WHEN_CHANGED);
    end();
    resize_cols();
  }
  void set(const std::vector<Node> &r) {
    rows_ = r;
    Fl_Table_Row::rows((int)rows_.size());
    select_all_rows(0);
    if (!rows_.empty()) select_row(0, 1);
    redraw();
  }
  std::vector<const Node *> selection() const {
    std::vector<const Node *> v;
    for (int r = 0; r < (int)rows_.size(); r++)
      if (const_cast<ArchiveView *>(this)->row_selected(r)) v.push_back(&rows_[(size_t)r]);
    return v;
  }
  void resize_cols() {
    int w0 = std::max(200, tiw - 110 - 160 - 150);
    col_width(0, w0);
    col_width(1, 110);
    col_width(2, 150);
    col_width(3, 160);
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Table_Row::resize(X, Y, W, H);
    resize_cols();
  }
  int handle(int e) override {
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key();
      if (k == FL_Enter || k == FL_KP_Enter) { activate_selected(); return 1; }
      if (k == FL_BackSpace) { go_up(); return 1; }
    }
    if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      // a row that is not selected yet becomes the selection
      int R = -1, C = -1;
      ResizeFlag rf;
      if (cursor2rowcol(R, C, rf) == CONTEXT_CELL && R >= 0 && !row_selected(R)) {
        select_all_rows(0);
        select_row(R, 1);
        Fl_Widget::do_callback();
      }
      take_focus();
      context_menu();
      return 1;
    }
    int r = Fl_Table_Row::handle(e);
    if (e == FL_PUSH && Fl::event_clicks() && Fl::event_button() == FL_LEFT_MOUSE &&
        callback_context() == CONTEXT_CELL) {
      Fl::event_clicks(0);
      activate_selected();
      return 1;
    }
    return r;
  }
  void go_up() {
    if (g_dir.empty()) return;
    std::string d = g_dir.find('/') == std::string::npos ? "" : g_dir.substr(0, g_dir.rfind('/'));
    show_dir(d);
  }
  void activate_selected();
  void context_menu();

protected:
  void draw_cell(TableContext ctx, int R, int C, int X, int Y, int W, int H) override {
    static const char *heads[] = {"Name", "Size", "Type", "Modified"};
    const fld::Palette &p = fld::palette();
    switch (ctx) {
      case CONTEXT_COL_HEADER:
        fl_push_clip(X, Y, W, H);
        fl_draw_box(FL_FLAT_BOX, X, Y, W, H, p.bg2);
        fl_color(p.border);
        fl_xyline(X, Y + H - 1, X + W);
        fl_color(p.fg_dim);
        fl_font(FL_HELVETICA_BOLD, fld::ui_font_size());
        fl_draw(heads[C], X + 8, Y, W - 8, H, FL_ALIGN_LEFT);
        fl_pop_clip();
        return;
      case CONTEXT_CELL: {
        if (R >= (int)rows_.size()) return;
        const Node &n = rows_[(size_t)R];
        bool sel = row_selected(R);
        fl_push_clip(X, Y, W, H);
        fl_color(sel ? p.selection : p.bg);
        fl_rectf(X, Y, W, H);
        fl_color(sel ? p.accent_fg : (C == 0 ? p.fg : p.fg_dim));
        fl_font(FL_HELVETICA, fld::ui_font_size());
        std::string t;
        int tx = X + 8;
        if (C == 0) {
          if (Fl_Image *ic = node_icon(n)) {
            ic->draw(X + 6, Y + (H - ic->h()) / 2);
            tx = X + 28;
          }
          t = fld::escape_at(n.name) + (n.encrypted ? "  🔒" : "");
        } else if (C == 1) {
          t = n.dir ? (std::to_string(n.items) + (n.items == 1 ? " item" : " items")) : fld::human_size((unsigned long long)n.size);
        } else if (C == 2) {
          t = n.dir ? "Folder" : fld::mime_description(fld::mime_from_name(n.name));
        } else {
          t = date_text(n.mtime);
        }
        fl_draw(t.c_str(), tx, Y, X + W - tx - 6, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
        fl_pop_clip();
        return;
      }
      default:
        return;
    }
  }
};

static std::vector<std::string> selected_paths();
static void extract_action();
static void delete_action();
static void add_files(bool folders);

void ArchiveView::context_menu() {
  bool open = !g_archive.empty();
  auto sel = selected_paths();
  bool one = sel.size() == 1;
  fld::Menu m;
  m.item("Open", [this] { activate_selected(); }, one ? 0 : FL_MENU_INACTIVE, FL_Enter);
  m.item(sel.empty() ? "Extract All…" : "Extract…", [] { extract_action(); }, open ? 0 : FL_MENU_INACTIVE,
         FL_CTRL + 'e');
  m.item("Delete from Archive", [] { delete_action(); }, open && g_writable && !sel.empty() ? 0 : FL_MENU_INACTIVE,
         FL_Delete).divider();
  m.item("Copy Name", [sel] {
    std::string t;
    for (auto &p : sel) t += (t.empty() ? "" : "\n") + fld::basename(p);
    Fl::copy(t.c_str(), (int)t.size(), 1);
  }, sel.empty() ? FL_MENU_INACTIVE : 0);
  m.item("Copy Path in Archive", [sel] {
    std::string t = fld::join(sel, "\n");
    Fl::copy(t.c_str(), (int)t.size(), 1);
  }, sel.empty() ? FL_MENU_INACTIVE : 0).divider();
  m.item("Select All", [this] {
    select_all_rows(1);
    Fl_Widget::do_callback();
  }, rows_.empty() ? FL_MENU_INACTIVE : 0, FL_CTRL + 'a');
  m.item("Go Up", [this] { go_up(); }, g_dir.empty() ? FL_MENU_INACTIVE : 0, FL_BackSpace).divider();
  m.item("Add Files…", [] { add_files(false); }, open && g_writable ? 0 : FL_MENU_INACTIVE);
  m.item("Add Folder…", [] { add_files(true); }, open && g_writable ? 0 : FL_MENU_INACTIVE);
  m.popup();
}

static void update_buttons() {
  bool open = !g_archive.empty();
  bool sel = g_view && !g_view->selection().empty();
  if (open) g_btn_extract->activate(); else g_btn_extract->deactivate();
  if (open && g_writable) { g_btn_add->activate(); g_btn_add_dir->activate(); }
  else { g_btn_add->deactivate(); g_btn_add_dir->deactivate(); }
  if (open && g_writable && sel) g_btn_delete->activate(); else g_btn_delete->deactivate();
  if (open && !g_dir.empty()) g_up->activate(); else g_up->deactivate();
}

static void show_dir(const std::string &dir) {
  g_dir = dir;
  g_view->set(g_tree.count(dir) ? g_tree[dir] : std::vector<Node>());
  std::string label = fld::basename(g_archive) + (dir.empty() ? "" : " › " + dir);
  g_path_box->copy_label(fld::escape_at(label).c_str());
  // status: the whole archive
  long files = 0;
  int64_t bytes = 0;
  for (auto &e : g_entries)
    if (!e.dir) {
      files++;
      bytes += e.size;
    }
  struct stat st;
  int64_t packed = stat(g_archive.c_str(), &st) == 0 ? st.st_size : 0;
  std::string s = std::to_string(files) + (files == 1 ? " file, " : " files, ") +
                  fld::human_size((unsigned long long)bytes) + " (" + fld::human_size((unsigned long long)packed) +
                  " packed)";
  if (!g_writable) s += " · read only";
  g_status->copy_label(s.c_str());
  update_buttons();
  g_win->redraw();
}

static void open_archive(const std::string &path, const std::string &password) {
  std::vector<arc::Entry> entries;
  arc::Result r = arc::list(path, entries, password);
  if (r.need_password) {
    std::string pw = ask_password(path, !password.empty());
    if (!pw.empty()) open_archive(path, pw);
    return;
  }
  if (!r.ok) {
    fl_alert("Could not open \"%s\":\n\n%s", fld::escape_at(fld::basename(path)).c_str(),
             fld::escape_at(r.error).c_str());
    return;
  }
  g_archive = path;
  g_password = password;
  g_entries = entries;
  // a password can also only be needed for the data: ask when extracting
  arc::Format f;
  g_writable = arc::format_for_name(path, f) && access(path.c_str(), W_OK) == 0 &&
               access(fld::dirname(path).c_str(), W_OK) == 0;
  build_tree();
  std::string t = fld::basename(path) + " – Archive Manager";
  g_win->copy_label(t.c_str());
  show_dir("");
}

static void reload() {
  std::string dir = g_dir;
  open_archive(g_archive, g_password);
  if (g_tree.count(dir)) show_dir(dir);
}

static std::string temp_dir() {
  std::string d = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-archive-" + std::to_string(getpid());
  fld::mkdir_p(d);
  return d;
}

void ArchiveView::activate_selected() {
  auto sel = selection();
  if (sel.size() != 1) return;
  const Node &n = *sel[0];
  if (n.dir) {
    show_dir(n.path);
    return;
  }
  // open a copy with its program
  auto req = std::make_shared<ExtractRequest>();
  req->archive = g_archive;
  req->password = g_password;
  req->dest = temp_dir();
  req->only = {n.path};
  req->strip = g_dir;
  std::string target = req->dest + "/" + n.name;
  unlink(target.c_str());
  req->after = [target](bool ok, const std::string &) {
    if (ok) fld::open_path(target);
  };
  start_extract(req);
}

// ---- actions ------------------------------------------------------------------------------

static std::vector<std::string> selected_paths() {
  std::vector<std::string> v;
  for (auto *n : g_view->selection()) v.push_back(n->path);
  return v;
}

static void extract_action() {
  if (g_archive.empty()) return;
  auto sel = selected_paths();
  // some items selected: those; otherwise everything (or the current folder)
  bool only = !sel.empty() && sel.size() < g_view->rows_.size();
  fld::FileDialogOptions o;
  o.title = only ? "Extract the Selected Items To" : "Extract To";
  o.directory = true;
  o.folder = fld::dirname(g_archive);
  o.accept_label = "Extract";
  auto res = fld::run_file_dialog(o);
  if (!res.ok || res.paths.empty()) return;
  auto req = std::make_shared<ExtractRequest>();
  req->archive = g_archive;
  req->password = g_password;
  req->dest = res.paths[0];
  if (only || !g_dir.empty()) {
    req->only = only ? sel : std::vector<std::string>{g_dir};
    req->strip = g_dir;
  }
  req->after = [](bool ok, const std::string &where) {
    if (ok && fl_choice("The files were extracted to\n%s", "Close", "Open Folder", nullptr,
                        fld::escape_at(where).c_str()) == 1)
      fld::open_path(where);
  };
  start_extract(req);
}

static void add_files(bool folders) {
  if (g_archive.empty() || !g_writable) return;
  fld::FileDialogOptions o;
  o.title = folders ? "Add a Folder" : "Add Files";
  o.multiple = !folders;
  o.directory = folders;
  o.accept_label = "Add";
  auto res = fld::run_file_dialog(o);
  if (!res.ok || res.paths.empty()) return;
  std::vector<std::string> files = res.paths;
  Job *j = new Job;
  j->title = "Adding to " + fld::basename(g_archive);
  std::string archive = g_archive, into = g_dir, pw = g_password;
  j->work = [archive, files, into, pw](const arc::Progress &p, Job &) { return arc::add(archive, files, into, pw, p); };
  j->done = [](const arc::Result &r) {
    if (!r.ok) show_error("Could not add the files.", r);
    reload();
  };
  run_job(j);
}

static void add_dropped(const std::vector<std::string> &files) {
  if (files.empty()) return;
  if (g_archive.empty() || !g_writable) {
    // nothing open: make a new archive of them
    compress_dialog(files, [](std::shared_ptr<CreateRequest> req) {
      if (!req) return;
      std::string a = req->archive;
      req->after = [a](bool ok) { if (ok) open_archive(a); };
      start_create(req);
    });
    return;
  }
  Job *j = new Job;
  j->title = "Adding to " + fld::basename(g_archive);
  std::string archive = g_archive, into = g_dir, pw = g_password;
  j->work = [archive, files, into, pw](const arc::Progress &p, Job &) { return arc::add(archive, files, into, pw, p); };
  j->done = [](const arc::Result &r) {
    if (!r.ok) show_error("Could not add the files.", r);
    reload();
  };
  run_job(j);
}

static void delete_action() {
  auto sel = selected_paths();
  if (sel.empty() || !g_writable) return;
  std::string what = sel.size() == 1 ? "\"" + fld::basename(sel[0]) + "\"" : std::to_string(sel.size()) + " items";
  if (fl_choice("Delete %s from the archive?", "Cancel", "Delete", nullptr, fld::escape_at(what).c_str()) != 1) return;
  Job *j = new Job;
  j->title = "Deleting from " + fld::basename(g_archive);
  std::string archive = g_archive, pw = g_password;
  j->work = [archive, sel, pw](const arc::Progress &p, Job &) { return arc::remove(archive, sel, pw, p); };
  j->done = [](const arc::Result &r) {
    if (!r.ok) show_error("Could not delete the items.", r);
    reload();
  };
  run_job(j);
}

static void open_action() {
  fld::FileDialogOptions o;
  o.title = "Open an Archive";
  fld::FileFilter f;
  f.name = "Archives";
  for (const char *g : {"*.zip", "*.7z", "*.tar", "*.tar.*", "*.tgz", "*.txz", "*.tbz2", "*.rar", "*.iso", "*.gz",
                        "*.xz", "*.zst", "*.bz2", "*.cpio", "*.deb", "*.rpm", "*.jar", "*.cab", "*.lha"})
    f.globs.push_back(g);
  fld::FileFilter all;
  all.name = "All files";
  all.globs = {"*"};
  o.filters = {f, all};
  auto res = fld::run_file_dialog(o);
  if (res.ok && !res.paths.empty()) open_archive(res.paths[0]);
}

static void new_action() {
  fld::FileDialogOptions o;
  o.title = "Choose What to Put into the New Archive";
  o.multiple = true;
  o.accept_label = "Choose";
  auto res = fld::run_file_dialog(o);
  if (!res.ok || res.paths.empty()) return;
  compress_dialog(res.paths, [](std::shared_ptr<CreateRequest> req) {
    if (!req) return;
    std::string a = req->archive;
    req->after = [a](bool ok) { if (ok) open_archive(a); };
    start_create(req);
  });
}

class MainWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  int handle(int e) override {
    switch (e) {
      case FL_DND_ENTER: case FL_DND_DRAG: case FL_DND_RELEASE: return 1;
      case FL_PASTE: {
        std::vector<std::string> files;
        for (auto &l : fld::split(std::string(Fl::event_text(), Fl::event_length()), '\n')) {
          std::string s = fld::trim(l);
          if (s.empty() || s[0] == '#') continue;
          if (fld::starts_with(s, "file://")) s = fld::uri_decode(s.substr(7));
          if (!s.empty() && s[0] == '/') files.push_back(s);
        }
        // one archive dropped on an empty window: open it
        if (g_archive.empty() && files.size() == 1 && arc::is_archive_name(files[0])) open_archive(files[0]);
        else add_dropped(files);
        return 1;
      }
      case FL_SHORTCUT:
        if (Fl::event_state(FL_CTRL)) {
          switch (Fl::event_key()) {
            case 'o': open_action(); return 1;
            case 'n': new_action(); return 1;
            case 'e': extract_action(); return 1;
            case 'q': hide(); return 1;
          }
        }
        if (Fl::event_key() == FL_Delete && g_writable) { delete_action(); return 1; }
        if (Fl::event_key() == FL_Escape) return 1;
        break;
    }
    return Fl_Double_Window::handle(e);
  }
};

static void cleanup_temp() {
  std::string d = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-archive-" + std::to_string(getpid());
  if (fld::is_dir(d)) fld::spawn({"rm", "-rf", d});
}

static int run_browser(const std::string &file) {
  const int W = 820, H = 540, TB = 44, PB = 34, SB = 26;
  g_win = new MainWindow(W, H, "Archive Manager");
  fld::Toolbar *tb = new fld::Toolbar(0, 0, W, TB);
  tb->add_button("document-new", "New archive (Ctrl+N)", [](Fl_Widget *, void *) { new_action(); }, nullptr, "New");
  tb->add_button("document-open", "Open an archive (Ctrl+O)", [](Fl_Widget *, void *) { open_action(); }, nullptr,
                 "Open");
  tb->add_separator();
  g_btn_extract = tb->add_button("document-save-as", "Extract the archive or the selected items (Ctrl+E)",
                                 [](Fl_Widget *, void *) { extract_action(); }, nullptr, "Extract");
  g_btn_add = tb->add_button("list-add", "Add files", [](Fl_Widget *, void *) { add_files(false); }, nullptr,
                             "Add Files");
  g_btn_add_dir = tb->add_button("folder", "Add a folder", [](Fl_Widget *, void *) { add_files(true); }, nullptr,
                                 "Add Folder");
  g_btn_delete = tb->add_button("edit-delete", "Delete the selected items from the archive (Delete)",
                                [](Fl_Widget *, void *) { delete_action(); }, nullptr, "Delete");
  tb->end();
  Fl_Group *pathbar = new Fl_Group(0, TB, W, PB);
  g_up = new fld::FlatButton(6, TB + 3, 28, 28);
  g_up->icon_name("go-up", 16);
  g_up->tooltip("Up one folder (Backspace)");
  g_up->callback([](Fl_Widget *, void *) { g_view->go_up(); });
  g_path_box = new Fl_Box(40, TB, W - 46, PB);
  g_path_box->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_path_box->labelfont(FL_HELVETICA_BOLD);
  pathbar->resizable(g_path_box);
  pathbar->end();
  g_view = new ArchiveView(0, TB + PB, W, H - TB - PB - SB);
  g_view->callback([](Fl_Widget *, void *) { update_buttons(); });
  g_status = new Fl_Box(0, H - SB, W, SB);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_status->labelsize(12);
  g_status->box(FL_FLAT_BOX);
  g_win->end();
  g_win->resizable(g_view);
  g_win->size_range(480, 300);
  fld::set_window_icon(g_win, "fl-archive");
  g_path_box->label("Open an archive, or drop files here to make a new one.");
  update_buttons();
  g_win->show();
  if (!file.empty()) open_archive(file);
  int r = Fl::run();
  cleanup_temp();
  return r;
}

// ---- command line modes ----------------------------------------------------------------------

static int g_pending = 0, g_failed = 0;

static void one_done(bool ok) {
  if (!ok) g_failed++;
  --g_pending; // run_batch() ends when it reaches 0
}

// runs the extractions one after the other
static void extract_files(std::vector<std::string> files, const std::string &dest, bool here) {
  if (files.empty()) return;
  std::string f = files.front();
  files.erase(files.begin());
  auto req = std::make_shared<ExtractRequest>();
  req->archive = f;
  req->here = here;
  req->dest = dest;
  req->after = [files, dest, here](bool ok, const std::string &) {
    one_done(ok);
    if (!files.empty()) extract_files(files, dest, here);
  };
  start_extract(req);
}

static int run_batch() {
  while (g_pending > 0 && Fl::wait() >= 0) {
  }
  if (g_progress_win) g_progress_win->hide();
  Fl::flush();
  cleanup_temp();
  return g_failed ? 1 : 0;
}

int main(int argc, char **argv) {
  fld::init_app("fl-archive", argc, argv);
  Fl::lock(); // Fl::awake from the worker threads
  std::string mode, dest;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-archive [ARCHIVE]\n"
             "       fl-archive --extract-here FILE...\n"
             "       fl-archive --extract-to DIR FILE...\n"
             "       fl-archive --extract-ask FILE...\n"
             "       fl-archive --compress FILE...\n");
      return 0;
    }
    if (a == "--extract-here" || a == "--extract-ask" || a == "--compress") mode = a;
    else if (a == "--extract-to" && i + 1 < argc) { mode = a; dest = argv[++i]; }
    else if (a == "-v" || a == "--version") { printf("fl-archive %s\n", FLD_VERSION); return 0; }
    else if (fld::starts_with(a, "file://")) files.push_back(fld::uri_decode(a.substr(7)));
    else if (!a.empty() && a[0] != '-') {
      if (a[0] != '/') {
        char cwd[4096];
        if (getcwd(cwd, sizeof cwd)) a = std::string(cwd) + "/" + a;
      }
      files.push_back(a);
    }
  }
  if (mode.empty()) return run_browser(files.empty() ? "" : files[0]);
  // no main window: the worker threads' messages (Fl::awake) need the
  // display open, and the progress window shows at once
  fl_open_display();
  g_batch = true;
  if (files.empty()) {
    fprintf(stderr, "fl-archive: %s needs files\n", mode.c_str());
    return 1;
  }
  if (mode == "--compress") {
    bool accepted = false;
    compress_dialog(files, [&accepted](std::shared_ptr<CreateRequest> req) {
      if (!req) return;
      accepted = true;
      g_pending = 1;
      req->after = [](bool ok) { one_done(ok); };
      start_create(req);
    });
    if (!accepted) return 0;
    return run_batch();
  }
  if (mode == "--extract-ask") {
    fld::FileDialogOptions o;
    o.title = "Extract To";
    o.directory = true;
    o.folder = fld::dirname(files[0]);
    o.accept_label = "Extract";
    auto res = fld::run_file_dialog(o);
    if (!res.ok || res.paths.empty()) return 0;
    dest = res.paths[0];
    mode = "--extract-to";
  }
  g_pending = (int)files.size();
  extract_files(files, dest, mode == "--extract-here");
  return run_batch();
}
