// Drag and drop rules and the progress UI for file operations
#include "fld/i18n.h"
#include "fld/fileops.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Progress.H>
#include <FL/fl_ask.H>

#include <algorithm>
#include <sys/stat.h>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

namespace fld {

std::vector<std::string> parse_uri_list(const std::string &text) {
  std::vector<std::string> out;
  for (auto &line : split(text, '\n')) {
    std::string l = trim(line);
    if (l.empty() || l[0] == '#') continue;
    if (starts_with(l, "file://")) {
      l = l.substr(7);
      if (starts_with(l, "localhost/")) l = l.substr(9);
      l = uri_decode(l);
    }
    if (l.empty() || l[0] != '/') continue;
    while (l.size() > 1 && l.back() == '/') l.pop_back();
    if (!file_exists(l) && !is_dir(l)) {
      struct stat st; // a dangling symlink can still be moved
      if (lstat(l.c_str(), &st) != 0) continue;
    }
    if (std::find(out.begin(), out.end(), l) == out.end()) out.push_back(l);
  }
  return out;
}

std::string make_uri_list(const std::vector<std::string> &paths) {
  std::string text;
  for (auto &p : paths) text += "file://" + uri_encode_path(p) + "\r\n";
  return text;
}

DropAction drop_action(const std::vector<std::string> &sources, const std::string &dest, int state) {
  if (sources.empty() || dest.empty()) return DROP_NONE;
  if (dest == "trash:///") return DROP_TRASH;
  std::string d = abs_path(dest);
  bool all_here = true;
  for (auto &s : sources) {
    std::string a = abs_path(s);
    if (d == a || starts_with(d + "/", a + "/")) return DROP_NONE; // into itself
    if (abs_path(dirname(s)) != d) all_here = false;
  }
  if (state & FL_CTRL) return DROP_COPY; // Ctrl+drop in the same folder duplicates
  if (all_here) return DROP_NONE;
  if (state & FL_SHIFT) return DROP_MOVE;
  struct stat ds, ss;
  if (stat(d.c_str(), &ds) != 0) return DROP_COPY;
  for (auto &s : sources)
    if (lstat(s.c_str(), &ss) != 0 || ss.st_dev != ds.st_dev) return DROP_COPY;
  return DROP_MOVE;
}

const char *drop_action_name(DropAction a) {
  switch (a) {
    case DROP_COPY: return "Copy";
    case DROP_MOVE: return "Move";
    case DROP_TRASH: return "Move to Trash";
    default: return "";
  }
}

namespace {

struct ProgressWin {
  Fl_Double_Window *win = nullptr;
  Fl_Progress *bar = nullptr;
  Fl_Box *label = nullptr;
  FileOp *op = nullptr;
  std::function<void(FileOp *)> done;
};

void show_errors(FileOp *op) {
  if (op->errors.empty()) return;
  std::string m;
  for (size_t i = 0; i < op->errors.size() && i < 8; i++) m += op->errors[i] + "\n";
  if (op->errors.size() > 8) m += "…and " + std::to_string(op->errors.size() - 8) + " more";
  fl_alert("%s", escape_at(m).c_str());
}

void progress_tick(void *d) {
  ProgressWin *p = (ProgressWin *)d;
  if (p->op->finished) return;
  double frac = p->op->total_bytes ? (double)p->op->done_bytes / p->op->total_bytes
                                   : (double)p->op->done_items / std::max<size_t>(1, p->op->sources.size());
  p->bar->value((float)(frac * 100));
  {
    std::lock_guard<std::mutex> l(p->op->mtx);
    std::string t = p->op->current;
    if (p->op->total_bytes) t += "   " + human_size(p->op->done_bytes) + " of " + human_size(p->op->total_bytes);
    p->label->copy_label(escape_at(t).c_str());
  }
  if (!p->win->shown() && p->op->total_bytes > (8u << 20)) p->win->show();
  Fl::repeat_timeout(0.2, progress_tick, d);
}

} // namespace

void run_file_op(FileOp::Kind k, const std::vector<std::string> &src, const std::string &dest,
                 std::function<void(FileOp *)> done) {
  if (src.empty()) return;
  FileOp *op = new FileOp;
  op->kind = k;
  op->sources = src;
  op->dest_dir = dest;
  if (k == FileOp::COPY || k == FileOp::MOVE) {
    int conflicts = 0;
    for (auto &s : src) {
      std::string d = path_join(dest, basename(s));
      if (file_exists(d) && abs_path(d) != abs_path(s)) conflicts++;
    }
    if (conflicts) {
      int c = fl_choice(_("%d item%s already exist%s in the destination."), _("Cancel"), _("Keep Both"), _("Replace"),
                        conflicts, conflicts == 1 ? "" : "s", conflicts == 1 ? "s" : "");
      if (c == 0) { delete op; return; }
      op->conflict = c == 1 ? FileOp::KEEP_BOTH : FileOp::REPLACE;
    }
  }
  ProgressWin *pw = new ProgressWin;
  pw->op = op;
  pw->done = std::move(done);
  const char *title = k == FileOp::COPY ? "Copying" : k == FileOp::MOVE ? "Moving"
                      : k == FileOp::TRASH ? "Moving to Trash" : "Deleting";
  pw->win = new Fl_Double_Window(420, 110, title);
  pw->label = new Fl_Box(12, 10, 396, 24);
  pw->label->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  pw->bar = new Fl_Progress(12, 40, 396, 20);
  pw->bar->minimum(0);
  pw->bar->maximum(100);
  pw->bar->selection_color(palette().accent);
  Fl_Button *cancel = new Fl_Button(308, 70, 100, 30, _("Cancel"));
  cancel->callback([](Fl_Widget *, void *d) { ((FileOp *)d)->cancel = true; }, op);
  pw->win->end();
  pw->win->set_non_modal();
  Fl::add_timeout(0.3, progress_tick, pw);
  op->start([pw](FileOp *o) {
    Fl::remove_timeout(progress_tick, pw);
    pw->win->hide();
    Fl::delete_widget(pw->win);
    show_errors(o);
    if (pw->done) pw->done(o);
    delete pw;
    // the FileOp is deleted after its worker has been joined
    Fl::add_timeout(0, [](void *d) { delete (FileOp *)d; }, o);
  });
}

void run_drop(DropAction a, const std::vector<std::string> &sources, const std::string &dest,
              std::function<void(FileOp *)> done) {
  switch (a) {
    case DROP_COPY: run_file_op(FileOp::COPY, sources, dest, std::move(done)); break;
    case DROP_MOVE: run_file_op(FileOp::MOVE, sources, dest, std::move(done)); break;
    case DROP_TRASH: run_file_op(FileOp::TRASH, sources, "", std::move(done)); break;
    default: break;
  }
}

} // namespace fld
