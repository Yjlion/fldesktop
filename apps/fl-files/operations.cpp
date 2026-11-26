// fl-files: undo, the properties and batch rename dialogs, opening and the context menus
#include "files_app.h"

// ---------------------------------------------------------------------------
// Operations

// ---- Undo (Ctrl+Z): renames, new folders and files, copies, moves and
// moving to the Trash. Deleting permanently can't be undone.
std::vector<UndoStep> g_undo;

static void push_undo(UndoStep::Kind k, std::vector<std::pair<std::string, std::string>> items) {
  if (items.empty()) return;
  g_undo.push_back({k, std::move(items)});
  if (g_undo.size() > 50) g_undo.erase(g_undo.begin());
}

const char *undo_label() {
  if (g_undo.empty()) return "&Undo";
  switch (g_undo.back().kind) {
    case UndoStep::RENAME: return "&Undo Rename";
    case UndoStep::CREATE: return "&Undo Create";
    case UndoStep::COPY: return "&Undo Copy";
    case UndoStep::MOVE: return "&Undo Move";
    case UndoStep::TRASH: return "&Undo Move to Trash";
  }
  return "&Undo";
}

// the item of the trash that `path` became (the latest one with that origin)
static bool trash_item_for(const std::string &path, fld::TrashItem &out) {
  bool found = false;
  for (auto &t : fld::trash_list())
    if (t.original_path == path && (!found || t.deletion_date >= out.deletion_date)) {
      out = t;
      found = true;
    }
  return found;
}

void undo() {
  if (g_undo.empty()) return;
  UndoStep st = g_undo.back();
  g_undo.pop_back();
  std::vector<std::string> errs;
  for (auto it = st.items.rbegin(); it != st.items.rend(); ++it) {
    const std::string &from = it->first, &to = it->second;
    std::string err;
    switch (st.kind) {
      case UndoStep::RENAME:
      case UndoStep::MOVE:
        // back where it was, unless something else took its place since
        if (!fld::file_exists(to)) errs.push_back(fld::basename(to) + " is no longer there");
        else if (fld::file_exists(from)) errs.push_back(fld::basename(from) + " already exists again");
        else if (!fld::move_path(to, from, &err)) errs.push_back(err);
        break;
      case UndoStep::COPY:
        // the copies go to the Trash: nothing is lost if they were changed
        if (fld::file_exists(to) && !fld::trash(to, &err)) errs.push_back(err);
        break;
      case UndoStep::CREATE: {
        struct stat sb;
        if (lstat(to.c_str(), &sb) != 0) break;
        bool empty = S_ISDIR(sb.st_mode) ? fld::list_dir(to, true).empty() : sb.st_size == 0;
        if (empty ? (S_ISDIR(sb.st_mode) ? rmdir(to.c_str()) : unlink(to.c_str())) != 0 : !fld::trash(to, &err))
          errs.push_back(err.empty() ? std::string(strerror(errno)) : err);
        break;
      }
      case UndoStep::TRASH: {
        fld::TrashItem t;
        if (!trash_item_for(from, t)) errs.push_back(fld::basename(from) + " is no longer in the Trash");
        else if (fld::file_exists(from)) errs.push_back(fld::basename(from) + " already exists again");
        else if (!fld::trash_restore(t, &err)) errs.push_back(err);
        break;
      }
    }
  }
  refresh();
  if (!errs.empty()) fl_alert("Could not undo everything:\n%s", fld::join(errs, "\n").c_str());
}

// a finished operation becomes an undo step (deleting permanently does not)
void record_op(FileOp *o) {
  if (o->kind == FileOp::DELETE) return;
  push_undo(o->kind == FileOp::COPY ? UndoStep::COPY : o->kind == FileOp::MOVE ? UndoStep::MOVE : UndoStep::TRASH,
            o->done);
}

void run_op(FileOp::Kind k, const std::vector<std::string> &src, const std::string &dest) {
  fld::run_file_op(k, src, dest, [](FileOp *o) {
    if (g_clip.cut && o->kind == FileOp::MOVE) { g_clip.paths.clear(); g_clip.cut = false; }
    record_op(o);
    refresh();
  });
}

void copy_to_clipboard(bool cut) {
  g_clip.paths = g_view->selected_paths();
  g_clip.cut = cut;
  std::string text;
  for (auto &p : g_clip.paths) text += "file://" + fld::uri_encode_path(p) + "\n";
  Fl::copy(text.c_str(), (int)text.size(), 1);
  g_view->redraw();
  update_status();
}

void paste_into(const std::string &dir) {
  if (in_trash() || in_network() || dir == TRASH_URI || (in_search() && dir == g_cwd)) return;
  if (g_clip.paths.empty()) {
    Fl::paste(*g_view, 1); // maybe file URIs from another program
    return;
  }
  run_op(g_clip.cut ? FileOp::MOVE : FileOp::COPY, g_clip.paths, dir);
}

void trash_selected(bool permanent) {
  if (in_network()) return;
  auto paths = g_view->selected_paths();
  if (paths.empty()) return;
  // network locations have no trash (moving there would copy everything home)
  if (!in_trash() && !permanent) {
    bool remote = false;
    for (auto &p : paths) remote = remote || is_remote(p);
    if (remote) {
      std::string what = paths.size() == 1 ? "\"" + fld::basename(paths[0]) + "\"" : std::to_string(paths.size()) + " items";
      if (fl_choice("Files on a network location cannot be moved to the Trash.\nDelete %s permanently?", "Cancel",
                    "Delete", nullptr, fld::escape_at(what).c_str()) != 1)
        return;
      run_op(FileOp::DELETE, paths, "");
      return;
    }
  }
  if (!in_trash() && !permanent && S.confirm_trash) {
    std::string what = paths.size() == 1 ? "\"" + fld::basename(paths[0]) + "\"" : std::to_string(paths.size()) + " items";
    if (fl_choice("Move %s to the Trash?", "Cancel", "Move to Trash", nullptr, fld::escape_at(what).c_str()) != 1) return;
  }
  if (in_trash() || permanent) {
    std::string what = paths.size() == 1 ? "\"" + fld::basename(paths[0]) + "\"" : std::to_string(paths.size()) + " items";
    if (fl_choice("Permanently delete %s?\nThis cannot be undone.", "Cancel", "Delete", nullptr, what.c_str()) != 1)
      return;
    if (in_trash()) {
      for (int i : g_view->sel) fld::trash_delete(g_view->entry(i).trash);
      refresh();
      return;
    }
    run_op(FileOp::DELETE, paths, "");
    return;
  }
  run_op(FileOp::TRASH, paths, "");
}

static void restore_selected() {
  std::string err;
  for (int i : g_view->sel)
    if (!fld::trash_restore(g_view->entry(i).trash, &err)) fl_alert("%s", err.c_str());
  refresh();
}

void new_item(bool folder) {
  if (in_trash() || in_search() || in_network()) return;
  std::string name = folder ? "New Folder" : "New File";
  if (!fld::ask_text(folder ? "New Folder" : "New File", folder ? "Folder name:" : "File name:", name, -1, "Create"))
    return;
  name = fld::trim(name);
  const char *n = name.c_str();
  if (!*n) return;
  if (strchr(n, '/')) { fl_alert("Names cannot contain '/'."); return; }
  std::string p = fld::path_join(g_cwd, n);
  if (fld::file_exists(p)) { fl_alert("\"%s\" already exists.", n); return; }
  bool ok = folder ? mkdir(p.c_str(), 0755) == 0 : fld::write_file(p, "");
  if (!ok) { fl_alert("Could not create \"%s\": %s", n, strerror(errno)); return; }
  push_undo(UndoStep::CREATE, {{"", p}});
  g_view->load(g_cwd, n);
}

void FileView::start_rename(int i) {
  if (i < 0 || in_trash() || in_network()) return;
  Entry &e = entry(i);
  // the name without its extension is selected: typing keeps ".jpg"
  std::string name = e.name;
  int stem = e.is_dir ? -1 : (int)frename::split_ext(e.name).first.size();
  if (!fld::ask_text("Rename", "New name:", name, stem, "Rename")) return;
  const char *n = name.c_str();
  if (!*n || e.name == n) return;
  if (strchr(n, '/')) { fl_alert("Names cannot contain '/'."); return; }
  std::string dst = fld::path_join(fld::dirname(e.path), n);
  if (fld::file_exists(dst) && strcasecmp(n, e.name.c_str()) != 0) { fl_alert("\"%s\" already exists.", n); return; }
  if (rename(e.path.c_str(), dst.c_str()) != 0) { fl_alert("Rename failed: %s", strerror(errno)); return; }
  push_undo(UndoStep::RENAME, {{e.path, dst}});
  if (in_search()) {
    e.path = dst;
    e.name = n;
    restat_all();
    return;
  }
  load(g_cwd, n);
}

// ---- properties dialog ------------------------------------------------------------------------

// the folder's read and write bits to everything inside (execute: folders only,
// files keep theirs); returns how many items failed
static int chmod_tree(const std::string &dir, mode_t m) {
  int failed = 0;
  DIR *d = opendir(dir.c_str());
  if (!d) return 1;
  while (struct dirent *de = readdir(d)) {
    std::string n = de->d_name;
    if (n == "." || n == "..") continue;
    std::string p = dir + "/" + n;
    struct stat st;
    if (lstat(p.c_str(), &st) != 0 || S_ISLNK(st.st_mode)) continue;
    mode_t want = S_ISDIR(st.st_mode) ? m : ((m & 0666) | (st.st_mode & 0111));
    if ((st.st_mode & 0777) != want && chmod(p.c_str(), (st.st_mode & 07000) | want) != 0) failed++;
    if (S_ISDIR(st.st_mode)) failed += chmod_tree(p, m);
  }
  closedir(d);
  return failed;
}

void properties(const std::vector<std::string> &paths_in) {
  std::vector<std::string> paths = paths_in;
  if (paths.empty()) paths.push_back(g_cwd);
  bool single = paths.size() == 1;
  struct stat st{};
  if (single) lstat(paths[0].c_str(), &st);
  Fl_Double_Window *w = new Fl_Double_Window(440, single ? 480 : 200, "Properties");
  int y = 16;
  auto row = [&](const char *k, const std::string &v) {
    Fl_Box *a = new Fl_Box(16, y, 110, 22, k);
    a->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
    a->labelcolor(fld::palette().fg_dim);
    Fl_Box *b = new Fl_Box(134, y, 290, 22);
    b->copy_label(fld::escape_at(v).c_str());
    b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    y += 26;
    return b;
  };
  std::string mime = single ? fld::mime_type(paths[0]) : "";
  if (single) {
    Fl_Box *ic = new Fl_Box(16, y, 48, 48);
    ic->image(fld::icon_first(fld::mime_icon_names(mime), 48));
    Fl_Box *nm = new Fl_Box(76, y, 348, 48);
    nm->copy_label(fld::escape_at(fld::basename(paths[0])).c_str());
    nm->labelfont(FL_HELVETICA_BOLD);
    nm->labelsize(16);
    nm->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    y += 60;
    row("Type:", fld::mime_description(mime) + " (" + mime + ")");
    row("Location:", fld::tilde(fld::dirname(paths[0])));
    if (S_ISLNK(st.st_mode)) {
      char t[4096];
      ssize_t n = readlink(paths[0].c_str(), t, sizeof t - 1);
      if (n > 0) { t[n] = 0; row("Link to:", t); }
    }
  } else {
    row("Selection:", std::to_string(paths.size()) + " items");
  }
  unsigned long long files = 0, dirs = 0, total = 0;
  for (auto &p : paths) total += tree_size(p, nullptr, &files, &dirs);
  std::string sz = fld::human_size(total) + " (" + std::to_string(total) + " bytes)";
  if (dirs) sz += ", " + std::to_string(files) + " files";
  row("Size:", sz);
  Fl_Check_Button *perm[9] = {};
  Fl_Choice *group = nullptr;
  Fl_Check_Button *recurse = nullptr;
  std::vector<gid_t> gids;
  bool mine = single && (st.st_uid == getuid() || geteuid() == 0) && !S_ISLNK(st.st_mode) && !is_remote(paths[0]);
  if (single) {
    row("Modified:", fld::format_time(st.st_mtime, "%Y-%m-%d %H:%M:%S"));
    row("Accessed:", fld::format_time(st.st_atime, "%Y-%m-%d %H:%M:%S"));
    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_gid);
    row("Owner:", pw ? pw->pw_name : std::to_string(st.st_uid));
    if (mine) {
      // the owner may give the file to any group they are in
      Fl_Box *gl = new Fl_Box(16, y, 110, 22, "Group:");
      gl->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
      gl->labelcolor(fld::palette().fg_dim);
      group = new Fl_Choice(134, y, 200, 24);
      std::vector<gid_t> list;
      if (geteuid() == 0) {
        setgrent();
        while (struct group *g = getgrent()) list.push_back(g->gr_gid);
        endgrent();
      } else {
        int n = getgroups(0, nullptr);
        list.resize(n > 0 ? (size_t)n : 0);
        if (n > 0) list.resize((size_t)std::max(0, getgroups(n, list.data())));
        list.push_back(getegid());
      }
      if (std::find(list.begin(), list.end(), st.st_gid) == list.end()) list.push_back(st.st_gid);
      std::sort(list.begin(), list.end());
      list.erase(std::unique(list.begin(), list.end()), list.end());
      for (gid_t g : list) {
        struct group *e = getgrgid(g);
        std::string name = e ? e->gr_name : std::to_string(g);
        std::string esc;
        for (char c : name) {
          if (c == '/' || c == '\\' || c == '&' || c == '_') esc += '\\';
          esc += c;
        }
        group->add(esc.c_str());
        if (g == st.st_gid) group->value((int)gids.size());
        gids.push_back(g);
      }
      y += 28;
    } else {
      row("Group:", gr ? gr->gr_name : std::to_string(st.st_gid));
    }
    y += 4;
    const char *who[] = {"Owner", "Group", "Others"};
    const char *what[] = {"Read", "Write", "Execute"};
    for (int r = 0; r < 3; r++) {
      Fl_Box *l = new Fl_Box(16, y, 110, 22, who[r]);
      l->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
      l->labelcolor(fld::palette().fg_dim);
      for (int c = 0; c < 3; c++) {
        perm[r * 3 + c] = new fld::CheckButton(134 + c * 95, y, 90, 22, what[c]);
        perm[r * 3 + c]->value((st.st_mode >> (8 - (r * 3 + c))) & 1);
        if (!mine) perm[r * 3 + c]->deactivate();
      }
      y += 26;
    }
    if (mine && S_ISDIR(st.st_mode)) {
      recurse = new fld::CheckButton(134, y, 290, 22, "Apply to everything inside");
      recurse->tooltip("Read and write go to all files and folders inside; execute to the folders only");
      y += 26;
    }
  }
  Fl_Return_Button *ok = new Fl_Return_Button(324, w->h() - 44, 100, 30, "Close");
  ok->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  w->end();
  w->set_modal();
  w->show();
  while (w->shown()) Fl::wait();
  if (single && perm[0] && perm[0]->active()) {
    mode_t m = st.st_mode & ~0777;
    for (int i = 0; i < 9; i++) if (perm[i]->value()) m |= 1 << (8 - i);
    if ((m & 07777) != (st.st_mode & 07777) && chmod(paths[0].c_str(), m & 07777) != 0)
      fl_alert("Could not change permissions: %s", strerror(errno));
    if (group && group->value() >= 0 && gids[(size_t)group->value()] != st.st_gid &&
        lchown(paths[0].c_str(), (uid_t)-1, gids[(size_t)group->value()]) != 0)
      fl_alert("Could not change the group: %s", strerror(errno));
    if (recurse && recurse->value()) {
      int failed = chmod_tree(paths[0], m & 0777);
      if (failed) fl_alert("%d items inside could not be changed.", failed);
    }
  }
  Fl::delete_widget(w);
  refresh();
}

// ---- batch rename ----------------------------------------------------------------------------

namespace {
struct RenameDlg {
  Fl_Input *pat, *find, *repl;
  Fl_Check_Button *regex, *icase;
  Fl_Choice *lcase;
  Fl_Spinner *start;
  Fl_Hold_Browser *list;
  Fl_Box *msg;
  Fl_Return_Button *ok;
  std::string dir;
  std::vector<std::string> olds, news;
  bool go = false;
};
} // namespace

static void rename_preview(Fl_Widget *, void *data) {
  RenameDlg &d = *(RenameDlg *)data;
  frename::Rule r;
  r.pattern = d.pat->value();
  r.find = d.find->value();
  r.replace = d.repl->value();
  r.regex = d.regex->value();
  r.ignore_case = d.icase->value();
  r.letter_case = (frename::Case)std::max(0, d.lcase->value());
  r.start = (int)d.start->value();
  std::string err;
  d.news = frename::apply(d.olds, r, &err);
  d.list->clear();
  std::set<std::string> olds(d.olds.begin(), d.olds.end());
  int bad = 0, changed = 0;
  if (d.news.empty()) {
    d.msg->copy_label(fld::escape_at(err).c_str());
    d.ok->deactivate();
    return;
  }
  auto why = frename::problems(d.olds, d.news);
  for (size_t i = 0; i < d.olds.size(); i++) {
    std::string w = why[i];
    struct stat st;
    if (w.empty() && d.news[i] != d.olds[i] && !olds.count(d.news[i]) && lstat((d.dir + "/" + d.news[i]).c_str(), &st) == 0)
      w = "already exists";
    if (!w.empty()) bad++;
    if (d.news[i] != d.olds[i]) changed++;
    std::string line = d.olds[i] + "\t" + d.news[i] + (w.empty() ? "" : "  (" + w + ")");
    d.list->add(line.c_str());
    if (!w.empty()) d.list->icon(d.list->size(), fld::icon_first({"dialog-warning"}, 16));
  }
  std::string m = bad ? std::to_string(bad) + (bad == 1 ? " name has a problem" : " names have problems")
                      : std::to_string(changed) + " of " + std::to_string(d.olds.size()) + " will be renamed";
  d.msg->copy_label(m.c_str());
  if (bad || !changed) d.ok->deactivate(); else d.ok->activate();
}

// F2 with more than one item selected: a pattern, find and replace and letter case,
// with a preview of the new names
void batch_rename(const std::vector<std::string> &paths) {
  if (paths.empty()) return;
  RenameDlg d;
  d.dir = fld::dirname(paths[0]);
  for (auto &p : paths) {
    if (fld::dirname(p) != d.dir) {
      fl_alert("Items from different folders cannot be renamed together.");
      return;
    }
    d.olds.push_back(fld::basename(p));
  }
  const int W = 600, H = 500;
  Fl_Double_Window *w = new Fl_Double_Window(W, H, "Rename Items");
  auto label = [](int y, const char *t) {
    Fl_Box *b = new Fl_Box(12, y, 96, 26, t);
    b->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  };
  label(12, "Name:");
  d.pat = new Fl_Input(114, 12, W - 126, 26);
  d.pat->value("{name}{ext}");
  d.pat->tooltip("{name} the name without its extension, {ext} the extension, {n} a number, {n:3} a number with three digits");
  label(44, "Find:");
  d.find = new Fl_Input(114, 44, (W - 126) / 2 - 4, 26);
  Fl_Box *rl = new Fl_Box(114 + (W - 126) / 2, 44, 70, 26, "Replace:");
  rl->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  d.repl = new Fl_Input(rl->x() + 74, 44, W - 12 - (rl->x() + 74), 26);
  d.regex = new fld::CheckButton(114, 74, 170, 24, "Regular expression");
  d.icase = new fld::CheckButton(290, 74, 120, 24, "Ignore case");
  label(104, "Case:");
  d.lcase = new Fl_Choice(114, 104, 160, 26);
  d.lcase->add("Keep|lower case|UPPER CASE|Title Case");
  d.lcase->value(0);
  Fl_Box *sl = new Fl_Box(290, 104, 120, 26, "Numbers from:");
  sl->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  d.start = new Fl_Spinner(414, 104, 90, 26);
  d.start->range(0, 1e9);
  d.start->step(1);
  d.start->value(1);
  d.list = new Fl_Hold_Browser(12, 142, W - 24, H - 142 - 84);
  static int cols[] = {(W - 24) / 2, 0};
  d.list->column_widths(cols);
  d.list->column_char('\t');
  d.list->format_char(0);
  d.msg = new Fl_Box(12, H - 76, W - 24, 24);
  d.msg->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  d.msg->labelcolor(fld::palette().fg_dim);
  Fl_Button *cancel = new Fl_Button(W - 224, H - 44, 100, 30, "Cancel");
  cancel->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  d.ok = new Fl_Return_Button(W - 116, H - 44, 104, 30, "Rename");
  d.ok->callback([](Fl_Widget *b, void *data) {
    ((RenameDlg *)data)->go = true;
    b->window()->hide();
  }, &d);
  for (Fl_Widget *x : std::initializer_list<Fl_Widget *>{d.pat, d.find, d.repl, d.regex, d.icase, d.lcase, d.start}) {
    x->callback(rename_preview, &d);
    x->when(FL_WHEN_CHANGED);
  }
  w->end();
  w->resizable(d.list);
  w->set_modal();
  rename_preview(nullptr, &d);
  w->show();
  d.pat->take_focus();
  while (w->shown()) Fl::wait();
  Fl::delete_widget(w);
  if (!d.go) return;
  auto steps = frename::plan(d.olds, d.news, ".fl-rename-" + std::to_string(getpid()) + "-");
  for (auto &st : steps) {
    std::string a = d.dir + "/" + st.first, b = d.dir + "/" + st.second;
    struct stat sb;
    // never overwrite (a file may have appeared since the preview)
    if (lstat(b.c_str(), &sb) == 0 || rename(a.c_str(), b.c_str()) != 0) {
      fl_alert("Could not rename \"%s\" to \"%s\": %s", fld::escape_at(st.first).c_str(),
               fld::escape_at(st.second).c_str(), errno ? strerror(errno) : "it exists");
      break;
    }
  }
  refresh();
}

// ---- open as administrator -------------------------------------------------------------------

// a folder in a new fl-files, a file in fl-edit, both as root through pkexec
static void open_as_admin(const std::string &path) {
  if (fld::which("pkexec").empty()) {
    fl_alert("pkexec (polkit) is needed to open items as the administrator.");
    return;
  }
  bool dir = fld::is_dir(path);
  std::string prog = fld::which(dir ? "fl-files" : "fl-edit");
  if (prog.empty()) prog = fld::which("fl-files");
  std::vector<std::string> argv{"pkexec", "env"};
  // pkexec keeps none of the session's variables: say where the display is
  const char *wd = getenv("WAYLAND_DISPLAY"), *rt = getenv("XDG_RUNTIME_DIR"), *xd = getenv("DISPLAY");
  if (wd && *wd) argv.push_back(std::string("WAYLAND_DISPLAY=") + (wd[0] == '/' || !rt ? wd : std::string(rt) + "/" + wd));
  if (xd && *xd) argv.push_back(std::string("DISPLAY=") + xd);
  if (const char *xa = getenv("XAUTHORITY")) argv.push_back(std::string("XAUTHORITY=") + xa);
  argv.push_back("XDG_RUNTIME_DIR=/tmp");
  argv.push_back(prog);
  argv.push_back(path);
  fld::spawn(argv);
}

// ---- open / open with ------------------------------------------------------------------------

static void open_with_dialog(const std::vector<std::string> &files) {
  if (files.empty()) return;
  std::string mime = fld::mime_type(files[0]);
  auto apps = fld::all_applications();
  Fl_Double_Window *w = new Fl_Double_Window(380, 420, "Open With");
  Fl_Box *l = new Fl_Box(12, 8, 356, 24);
  std::string lbl = "Open \"" + fld::basename(files[0]) + "\" with:";
  l->copy_label(fld::escape_at(lbl).c_str());
  l->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  Fl_Hold_Browser *b = new Fl_Hold_Browser(12, 36, 356, 300);
  for (auto &a : apps) {
    b->add(fld::escape_at(a.name).c_str());
    b->icon(b->size(), fld::icon_first({a.icon, "application-x-executable"}, 20));
  }
  Fl_Check_Button *def = new fld::CheckButton(12, 342, 356, 24, "Always use for this file type");
  Fl_Button *cancel = new Fl_Button(160, 378, 100, 30, "Cancel");
  Fl_Return_Button *ok = new Fl_Return_Button(268, 378, 100, 30, "Open");
  static int result;
  result = 0;
  cancel->callback([](Fl_Widget *x, void *) { result = 0; x->window()->hide(); });
  ok->callback([](Fl_Widget *x, void *) { result = 1; x->window()->hide(); });
  b->callback([](Fl_Widget *x, void *) { if (Fl::event_clicks()) { result = 1; x->window()->hide(); } });
  w->end();
  w->set_modal();
  w->show();
  while (w->shown()) Fl::wait();
  int line = b->value();
  if (result && line > 0) {
    const fld::DesktopEntry &app = apps[line - 1];
    if (def->value()) fld::set_default_app_for_mime(mime, app.id);
    app.launch(files);
  }
  Fl::delete_widget(w);
}

void open_entry(Entry &e) {
  if (!e.target.empty()) { // network view: a computer or share
    mount_and_go(e.target);
    return;
  }
  if (in_trash()) return;
  if (e.is_dir) {
    navigate(e.path);
    return;
  }
  std::string m = e.get_mime();
  if (m == "application/x-executable" || (m == "application/x-shellscript" && (e.mode & S_IXUSR))) {
    int c = fl_choice("\"%s\" is a program. Run it or open it as text?", "Cancel", "Run", "Open",
                      e.name.c_str());
    if (c == 1) { fld::spawn({e.path}, g_cwd); return; }
    if (c == 0) return;
    if (m == "application/x-executable") return;
    fld::spawn({"fl-edit", e.path});
    return;
  }
  if (!fld::open_path(e.path)) open_with_dialog({e.path});
}

// ---- menus --------------------------------------------------------------------------------------


void FileView::context_menu(int i) {
  if (!sel.count(i)) select_only(i);
  Entry &e = entry(i);
  if (!e.target.empty()) {
    std::string t = e.target;
    fld::Menu m;
    m.item("Open", [t] { mount_and_go(t); });
    m.item("Open in New Tab", [t] { mount_and_go(t, 2); });
    m.item("Open in New Window", [t] { fld::spawn({"fl-files", t}); }, FL_MENU_DIVIDER);
    m.item("Copy Address", [t] {
      Fl::copy(t.c_str(), (int)t.size(), 1);
      Fl::copy(t.c_str(), (int)t.size(), 0);
    });
    m.popup();
    return;
  }
  std::vector<std::string> paths = selected_paths();
  bool many = paths.size() > 1;
  std::vector<fld::DesktopEntry> apps;
  if (!e.is_dir && !many && !in_trash()) apps = fld::apps_for_mime(e.get_mime());
  std::vector<Fl_Menu_Item> m;
  enum { OPEN = 1, OPEN_TAB, OPEN_TERM, OPEN_WITH_OTHER, CUT, COPY, PASTE, RENAME, TRASH, DELETE, PROPS,
         RESTORE, COPY_PATH, CONTAINING, BOOKMARK, NEW_WINDOW, EXTRACT_HERE, EXTRACT_TO, COMPRESS, BATCH_RENAME,
         ADMIN, COPY_OTHER, MOVE_OTHER, APP = 100 };
  std::string other = other_pane_dir();
  bool local = !is_remote(e.path) && geteuid() != 0;
  // archives: extract / compress with fl-archive
  bool have_archiver = !in_trash() && !fld::which("fl-archive").empty();
  bool all_archives = have_archiver && !paths.empty();
  for (int k : sel) all_archives = all_archives && !entry(k).is_dir && fld::is_archive_mime(entry(k).get_mime());
  if (in_trash()) {
    m.push_back({"Restore", 0, nullptr, (void *)RESTORE});
    m.push_back({"Delete Permanently", 0, nullptr, (void *)DELETE});
  } else {
    m.push_back({"Open", 0, nullptr, (void *)OPEN});
    if (in_search() && !many) m.push_back({"Open Containing Folder", 0, nullptr, (void *)CONTAINING});
    if (e.is_dir) m.push_back({"Open in New Tab", 0, nullptr, (void *)OPEN_TAB});
    if (e.is_dir) m.push_back({"Open in New Window", 0, nullptr, (void *)NEW_WINDOW});
    if (e.is_dir) m.push_back({"Open in Terminal", 0, nullptr, (void *)OPEN_TERM});
    if (!many && local) m.push_back({"Open as Administrator", 0, nullptr, (void *)ADMIN});
    if (e.is_dir && !many) m.push_back({"Add to Bookmarks", 0, nullptr, (void *)BOOKMARK});
    if (!e.is_dir && !many) {
      m.push_back({"Open With", 0, nullptr, nullptr, FL_SUBMENU});
      for (size_t k = 0; k < apps.size() && k < 12; k++) m.push_back({apps[k].name.c_str(), 0, nullptr, (void *)(APP + k)});
      if (!apps.empty()) m.back().flags |= FL_MENU_DIVIDER;
      m.push_back({"Other Application…", 0, nullptr, (void *)OPEN_WITH_OTHER});
      m.push_back({nullptr});
    }
    m.back().flags |= FL_MENU_DIVIDER;
    if (all_archives) {
      m.push_back({"Extract Here", 0, nullptr, (void *)EXTRACT_HERE});
      m.push_back({"Extract To…", 0, nullptr, (void *)EXTRACT_TO});
    }
    if (have_archiver) m.push_back({"Compress…", 0, nullptr, (void *)COMPRESS, FL_MENU_DIVIDER});
    m.push_back({"Cut", FL_CTRL + 'x', nullptr, (void *)CUT});
    m.push_back({"Copy", FL_CTRL + 'c', nullptr, (void *)COPY});
    if (e.is_dir && !many) m.push_back({"Paste Into Folder", 0, nullptr, (void *)PASTE, g_clip.paths.empty() ? FL_MENU_INACTIVE : 0});
    m.push_back({"Copy Path", 0, nullptr, (void *)COPY_PATH, FL_MENU_DIVIDER});
    if (!other.empty() && other != g_cwd) {
      m.push_back({"Copy to Other Pane", 0, nullptr, (void *)COPY_OTHER});
      m.push_back({"Move to Other Pane", 0, nullptr, (void *)MOVE_OTHER, FL_MENU_DIVIDER});
    }
    if (!many) m.push_back({"Rename…", FL_F + 2, nullptr, (void *)RENAME});
    else if (!in_search()) m.push_back({"Rename…", FL_F + 2, nullptr, (void *)BATCH_RENAME});
    m.push_back({"Move to Trash", FL_Delete, nullptr, (void *)TRASH});
    m.push_back({"Delete Permanently", FL_SHIFT + FL_Delete, nullptr, (void *)DELETE, FL_MENU_DIVIDER});
    m.push_back({"Properties", FL_ALT + FL_Enter, nullptr, (void *)PROPS});
  }
  m.push_back({nullptr});
  const Fl_Menu_Item *r = m[0].popup(Fl::event_x(), Fl::event_y());
  if (!r) return;
  long id = (long)r->user_data();
  switch (id) {
    case OPEN:
      for (int k : std::set<int>(sel)) open_entry(entry(k));
      break;
    case OPEN_TERM: fld::spawn(fld::shell_split(fld::terminal_command()), e.path); break;
    case NEW_WINDOW: fld::spawn({"fl-files", e.path}); break;
    case OPEN_TAB:
      for (int k : std::set<int>(sel))
        if (entry(k).is_dir) new_tab(entry(k).path, false);
      break;
    case CONTAINING: open_containing(e.path); break;
    case BOOKMARK: add_bookmark(e.path); break;
    case OPEN_WITH_OTHER: open_with_dialog(paths); break;
    case CUT: copy_to_clipboard(true); break;
    case COPY: copy_to_clipboard(false); break;
    case PASTE: paste_into(e.path); break;
    case COPY_PATH: {
      std::string t = fld::join(paths, "\n");
      Fl::copy(t.c_str(), (int)t.size(), 1);
      Fl::copy(t.c_str(), (int)t.size(), 0);
      break;
    }
    case RENAME: start_rename(i); break;
    case BATCH_RENAME: batch_rename(paths); break;
    case ADMIN: open_as_admin(e.path); break;
    case COPY_OTHER: run_op(FileOp::COPY, paths, other); break;
    case MOVE_OTHER: run_op(FileOp::MOVE, paths, other); break;
    case TRASH: trash_selected(false); break;
    case DELETE: trash_selected(true); break;
    case PROPS: properties(paths); break;
    case RESTORE: restore_selected(); break;
    case EXTRACT_HERE:
    case EXTRACT_TO:
    case COMPRESS: {
      std::vector<std::string> argv{"fl-archive", id == EXTRACT_HERE ? "--extract-here"
                                                  : id == EXTRACT_TO ? "--extract-ask"
                                                                     : "--compress"};
      argv.insert(argv.end(), paths.begin(), paths.end());
      fld::spawn(argv, g_cwd);
      break;
    }
    default:
      if (id >= APP) apps[id - APP].launch(paths);
  }
}



void FileView::background_menu() {
  sel.clear();
  redraw();
  update_status();
  if (in_network()) {
    fld::Menu m;
    m.item("Connect to Server…", [] { connect_to_server(); });
    m.item("Reload", [] { refresh(); }, 0, FL_F + 5);
    m.popup();
    return;
  }
  if (in_trash()) {
    Fl_Menu_Item m[] = {{"Empty Trash", 0, nullptr, (void *)1}, {nullptr}};
    const Fl_Menu_Item *r = m->popup(Fl::event_x(), Fl::event_y());
    if (r && fl_choice("Permanently delete all items in the Trash?", "Cancel", "Empty Trash", nullptr) == 1) {
      fld::trash_empty();
      refresh();
    }
    return;
  }
  bool search = in_search();
  Fl_Menu_Item m[] = {
      {"New Folder…", FL_CTRL + FL_SHIFT + 'n', nullptr, (void *)1, search ? FL_MENU_INACTIVE : 0},
      {"New Empty File…", 0, nullptr, (void *)2, FL_MENU_DIVIDER | (search ? FL_MENU_INACTIVE : 0)},
      {"Paste", FL_CTRL + 'v', nullptr, (void *)3, FL_MENU_DIVIDER},
      {"Open Terminal Here", 0, nullptr, (void *)4},
      {"Open as Administrator", 0, nullptr, (void *)8, search || is_remote(g_cwd) || geteuid() == 0 ? FL_MENU_INACTIVE : 0},
      {"Select All", FL_CTRL + 'a', nullptr, (void *)5, FL_MENU_DIVIDER},
      {"Sort By", 0, nullptr, nullptr, FL_SUBMENU},
        {"Name", 0, nullptr, (void *)10, FL_MENU_RADIO | (V.sort == SORT_NAME ? FL_MENU_VALUE : 0)},
        {"Size", 0, nullptr, (void *)11, FL_MENU_RADIO | (V.sort == SORT_SIZE ? FL_MENU_VALUE : 0)},
        {"Type", 0, nullptr, (void *)12, FL_MENU_RADIO | (V.sort == SORT_TYPE ? FL_MENU_VALUE : 0)},
        {"Modified", 0, nullptr, (void *)13, FL_MENU_RADIO | (V.sort == SORT_DATE ? FL_MENU_VALUE : 0)},
        {"Permissions", 0, nullptr, (void *)14, FL_MENU_RADIO | (V.sort == SORT_PERMS ? FL_MENU_VALUE : 0)},
        {"Owner", 0, nullptr, (void *)15, FL_MENU_RADIO | (V.sort == SORT_OWNER ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER},
        {"Descending", 0, nullptr, (void *)19, FL_MENU_TOGGLE | (V.desc ? FL_MENU_VALUE : 0)},
        {"Folders First", 0, nullptr, (void *)18, FL_MENU_TOGGLE | (S.folders_first ? FL_MENU_VALUE : 0)},
        {nullptr},
      {"View", 0, nullptr, nullptr, FL_SUBMENU},
        {"Icons", FL_CTRL + '1', nullptr, (void *)20, FL_MENU_RADIO | (V.view == VIEW_ICONS ? FL_MENU_VALUE : 0)},
        {"Compact", FL_CTRL + '2', nullptr, (void *)21, FL_MENU_RADIO | (V.view == VIEW_COMPACT ? FL_MENU_VALUE : 0)},
        {"Details", FL_CTRL + '3', nullptr, (void *)22, FL_MENU_RADIO | (V.view == VIEW_DETAILS ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER},
        {"Zoom In", FL_CTRL + '=', nullptr, (void *)23, V.zoom >= kZoomMax ? FL_MENU_INACTIVE : 0},
        {"Zoom Out", FL_CTRL + '-', nullptr, (void *)24, V.zoom <= 0 ? FL_MENU_INACTIVE : 0},
        {"Normal Size", FL_CTRL + '0', nullptr, (void *)25, FL_MENU_DIVIDER},
        {"Columns…", 0, nullptr, (void *)26},
        {nullptr},
      {"Show Hidden Files", FL_CTRL + 'h', nullptr, (void *)6, FL_MENU_TOGGLE | (S.show_hidden ? FL_MENU_VALUE : 0)},
      {"Properties", 0, nullptr, (void *)7},
      {nullptr}};
  if (g_clip.paths.empty() || search) m[2].flags |= FL_MENU_INACTIVE;
  const Fl_Menu_Item *r = m->popup(Fl::event_x(), Fl::event_y());
  if (!r) return;
  long id = (long)r->user_data();
  switch (id) {
    case 1: new_item(true); break;
    case 2: new_item(false); break;
    case 3: paste_into(g_cwd); break;
    case 4: fld::spawn(fld::shell_split(fld::terminal_command()), g_cwd); break;
    case 5: for (int i = 0; i < (int)items.size(); i++) sel.insert(i); redraw(); update_status(); break;
    case 6: toggle_hidden(); break;
    case 7: properties({g_cwd}); break;
    case 8: open_as_admin(g_cwd); break;
    case 10: case 11: case 12: case 13: case 14: case 15: {
      static const SortKey k[] = {SORT_NAME, SORT_SIZE, SORT_TYPE, SORT_DATE, SORT_PERMS, SORT_OWNER};
      set_sort(k[id - 10], V.desc);
      break;
    }
    case 18: S.folders_first = !S.folders_first; save_settings(); resort_keep_selection(); break;
    case 19: set_sort(V.sort, !V.desc); break;
    case 20: set_view(VIEW_ICONS); break;
    case 21: set_view(VIEW_COMPACT); break;
    case 22: set_view(VIEW_DETAILS); break;
    case 23: zoom(1); break;
    case 24: zoom(-1); break;
    case 25: zoom(0); break;
    case 26: columns_menu(Fl::event_x(), Fl::event_y()); break;
  }
}

int FileView::handle(int e) {
  if (g_split && this != g_view) {
    // a click, a drop or the keyboard focus in the other pane makes it current
    if (e == FL_PUSH || e == FL_FOCUS || e == FL_DND_ENTER || e == FL_MOUSEWHEEL) {
      int i = tab_index(tab_of(this));
      if (i >= 0) activate_tab(i);
    }
    if (this != g_view) {
      PaneGuard guard(this);
      return handle_view(e);
    }
  }
  return handle_view(e);
}

int FileView::handle_view(int e) {
  if (sb->visible() && Fl::event_inside(sb) && e != FL_KEYBOARD && e != FL_MOUSEWHEEL) return Fl_Group::handle(e);
  switch (e) {
    case FL_FOCUS: case FL_UNFOCUS: redraw(); return 1;
    case FL_MOUSEWHEEL:
      if (Fl::event_state() & FL_CTRL) {
        if (Fl::event_dy()) zoom(Fl::event_dy() < 0 ? 1 : -1);
        return 1;
      }
      top = std::max(0, std::min(top + Fl::event_dy() * (V.view == VIEW_ICONS ? cell_h() / 2 : row_h() * 3),
                                 content_height() - view_h()));
      update_scrollbar();
      redraw();
      return 1;
    case FL_PUSH: {
      take_focus();
      int ex = Fl::event_x(), ey = Fl::event_y();
      if (details() && ey < y() + HEADER_H) {
        bool border;
        int c = header_hit(ex, border);
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          columns_menu(ex, ey);
          return 1;
        }
        if (border && c >= 0) {
          // drag a column border to resize it
          resizing_col = c;
          resize_x0 = ex;
          resize_w0 = S.col_w[c];
          return 1;
        }
        if (c == -2 || border) return 1;
        SortKey k = c < 0 ? SORT_NAME : kColSort[c];
        set_sort(k, V.sort == k ? !V.desc : (k == SORT_SIZE || k == SORT_DATE));
        return 1;
      }
      int i = item_at(ex, ey);
      int st = Fl::event_state();
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        if (i >= 0) context_menu(i);
        else background_menu();
        return 1;
      }
      if (Fl::event_button() == FL_MIDDLE_MOUSE) {
        if (i >= 0 && entry(i).is_dir) open_dir_elsewhere(entry(i).path);
        return 1;
      }
      press_item = i;
      press_x = ex;
      press_y = ey;
      pending_toggle = pending_single = -1;
      end_drag();
      if (hover_item != -1) hover_item = -1;
      if (i >= 0) {
        if (st & FL_CTRL) {
          // a selected item is deselected on release (unless it is dragged)
          if (sel.count(i)) pending_toggle = i; else sel.insert(i);
          focus = anchor = i;
        } else if (st & FL_SHIFT && anchor >= 0) {
          sel.clear();
          for (int k = std::min(anchor, i); k <= std::max(anchor, i); k++) sel.insert(k);
          focus = i;
        } else if (!sel.count(i)) {
          sel = {i};
          focus = anchor = i;
        } else {
          focus = i;
          if (sel.size() > 1) pending_single = i;
        }
        if (Fl::event_clicks() && !(st & (FL_CTRL | FL_SHIFT)) && !S.single_click) {
          Fl::event_clicks(0);
          open_entry(entry(i));
          return 1;
        }
      } else {
        if (!(st & (FL_CTRL | FL_SHIFT))) sel.clear();
        banding = true;
        bx0 = bx1 = ex;
        by0 = by1 = ey + top;
      }
      redraw();
      update_status();
      return 1;
    }
    case FL_DRAG: {
      int ex = Fl::event_x(), ey = Fl::event_y();
      if (resizing_col >= 0) {
        S.col_w[resizing_col] = std::max(40, std::min(1000, resize_w0 + ex - resize_x0));
        redraw();
        return 1;
      }
      if (banding) {
        bx1 = ex;
        by1 = ey + top;
        if (ey < view_top()) { top = std::max(0, top - 10); update_scrollbar(); }
        else if (ey > y() + h()) { top = std::min(std::max(0, content_height() - view_h()), top + 10); update_scrollbar(); }
        if (!(Fl::event_state() & FL_CTRL)) sel.clear();
        int rx0 = std::min(bx0, bx1), rx1 = std::max(bx0, bx1);
        int ry0 = std::min(by0, by1) - top, ry1 = std::max(by0, by1) - top;
        for (int i = 0; i < (int)items.size(); i++) {
          int ix, iy, iw, ih;
          item_rect(i, ix, iy, iw, ih);
          if (ix + iw > rx0 && ix < rx1 && iy + ih > ry0 && iy < ry1) sel.insert(i);
        }
        redraw();
        update_status();
        return 1;
      }
      if (press_item >= 0 && !dragging_files && !in_trash() && !in_network() &&
          (Fl::event_state() & FL_BUTTON1) && (std::abs(ex - press_x) > 6 || std::abs(ey - press_y) > 6)) {
        if (pending_toggle >= 0) { sel.insert(pending_toggle); pending_toggle = -1; }
        pending_single = -1;
        auto paths = selected_paths();
        if (paths.empty()) return 1;
        // start a drag with file URIs and their icons; the dragged items
        // fade until it ends
        fade_step = 0;
        ghost_w = 0.4f;
        clear_ghosts();
        dragging_files = true;
        drop_item = hover_item = -1;
        redraw();
        update_status();
        fld::DragImage img;
        img.size = std::max(32, std::min(64, big()));
        std::vector<int> order;
        if (sel.count(press_item)) order.push_back(press_item);
        for (int k : sel)
          if (k != press_item && order.size() < 3) order.push_back(k);
        for (int k : order)
          if (Fl_Image *ic = icon_for(entry(k), true)) img.icons.push_back(ic);
        img.count = drag_count = (int)paths.size();
        std::string text = fld::make_uri_list(paths);
        FileView *self = this;
        if (!fld::start_drag(window(), text, true, img, [self](fld::DragEnd e) {
              if (live().count(self)) self->drag_over(e);
            })) {
          Fl::copy(text.c_str(), (int)text.size(), 0);
          Fl::dnd();
        }
      }
      return 1;
    }
    case FL_RELEASE:
      if (resizing_col >= 0) {
        resizing_col = -1;
        save_settings();
        return 1;
      }
      if (banding) {
        banding = false;
        redraw();
      }
      // a click (no drag) on a selected item: Ctrl deselects it, a plain
      // click selects only it
      if (!dragging_files && press_item >= 0 && press_item < (int)items.size()) {
        if (pending_toggle == press_item) {
          sel.erase(press_item);
          redraw();
          update_status();
        } else if (pending_single == press_item && !(Fl::event_state() & (FL_CTRL | FL_SHIFT))) {
          sel = {press_item};
          anchor = press_item;
          redraw();
          update_status();
        }
      }
      pending_toggle = pending_single = -1;
      // single-click mode opens on release (not after a drag or with modifiers)
      if (S.single_click && Fl::event_button() == FL_LEFT_MOUSE && press_item >= 0 && !dragging_files &&
          !(Fl::event_state() & (FL_CTRL | FL_SHIFT)) && press_item == item_at(Fl::event_x(), Fl::event_y()) &&
          press_item < (int)items.size()) {
        int i = press_item;
        press_item = -1;
        open_entry(entry(i));
      }
      return 1;
    case FL_ENTER:
      end_drag(); // pointer events come back once a drag is over
      return 1;
    case FL_MOVE: {
      // the pointer is not grabbed while files are dragged: a drag is over
      end_drag();
      int hi = item_at(Fl::event_x(), Fl::event_y());
      if (hi != hover_item) { hover_item = hi; redraw(); }
      if (details() && Fl::event_y() < y() + HEADER_H) {
        bool border;
        int c = header_hit(Fl::event_x(), border);
        window()->cursor(border && c >= 0 ? FL_CURSOR_WE : FL_CURSOR_DEFAULT);
      } else window()->cursor(FL_CURSOR_DEFAULT);
      return 1;
    }
    case FL_LEAVE:
      if (hover_item != -1) { hover_item = -1; redraw(); }
      if (window()) window()->cursor(FL_CURSOR_DEFAULT);
      return 1;
    case FL_DND_ENTER: case FL_DND_DRAG: {
      if (in_trash() || in_network()) return 0;
      // highlight the folder the files would go into
      int ey = Fl::event_y();
      int i = item_at(Fl::event_x(), ey);
      if (i >= 0 && !entry(i).is_dir) i = -1;
      if (i >= 0 && dragging_files && sel.count(i)) i = -1; // not onto itself
      if (i != drop_item) { drop_item = i; redraw(); }
      spring(i);
      set_scroll_dir(ey < view_top() + 24 ? -1 : ey > y() + h() - 24 ? 1 : 0);
      if (dragging_files) update_status();
      return 1;
    }
    case FL_DND_RELEASE:
      stop_drop_effects();
      if (in_trash() || in_network()) return 0;
      dropping = true;
      return 1;
    case FL_DND_LEAVE:
      stop_drop_effects();
      if (drop_item != -1) { drop_item = -1; redraw(); }
      if (dragging_files) update_status();
      return 1;
    case FL_PASTE: {
      // dropped or pasted file URIs: into the folder under the pointer (drops)
      // or here; moved within a file system, else copied (Ctrl copies, Shift moves)
      bool drop = dropping || dragging_files;
      dropping = false;
      int target = drop_item;
      if (drop_item != -1) { drop_item = -1; redraw(); }
      if (in_trash() || in_network()) return 1;
      auto paths = fld::parse_uri_list(std::string(Fl::event_text(), Fl::event_length()));
      if (paths.empty()) { end_drag(); return 1; }
      std::string dest = g_cwd;
      if (drop && target >= 0 && target < (int)items.size() && entry(target).is_dir) dest = entry(target).path;
      else if (in_search()) { end_drag(); return 1; }
      fld::DropAction a = drop ? fld::drop_action(paths, dest, Fl::event_state())
                               : fld::drop_action(paths, dest, FL_CTRL); // a paste copies
      end_drag();
      if (!drop && a == fld::DROP_NONE) return 1;
      if (drop && a != fld::DROP_NONE && dest != g_cwd) pulse(dest);
      fld::run_drop(a, paths, dest, [](FileOp *o) { record_op(o); refresh(); });
      return 1;
    }
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      int st = Fl::event_state();
      bool shift = st & FL_SHIFT, ctrl = st & FL_CTRL;
      int cols = columns();
      int page = std::max(1, view_h() / (details() ? row_h() : cell_h())) * cols;
      // tabs: Ctrl+Tab, Ctrl+Shift+Tab, Ctrl+Page Up/Down
      if (ctrl && (k == FL_Tab || k == FL_Page_Up || k == FL_Page_Down) && g_tabs.size() > 1) {
        int n = (int)g_tabs.size(), d = (k == FL_Page_Up || (k == FL_Tab && shift)) ? -1 : 1;
        activate_tab((g_cur + d + n) % n);
        return 1;
      }
      switch (k) {
        case FL_Up: move_focus(-cols, shift); return 1;
        case FL_Down: move_focus(cols, shift); return 1;
        case FL_Left: if (details()) return 0; move_focus(-1, shift); return 1;
        case FL_Right: if (details()) return 0; move_focus(1, shift); return 1;
        case FL_Page_Up: move_focus(-page, shift); return 1;
        case FL_Page_Down: move_focus(page, shift); return 1;
        case FL_Home: move_focus(-(int)items.size(), shift); return 1;
        case FL_End: move_focus((int)items.size(), shift); return 1;
        case FL_Enter: case FL_KP_Enter:
          if (st & FL_ALT) { properties(selected_paths()); return 1; }
          for (int i : std::set<int>(sel)) open_entry(entry(i));
          return 1;
        case FL_Delete: if (!in_network()) trash_selected(shift); return 1;
        case FL_F + 2:
          if (sel.size() > 1 && !in_network() && !in_trash() && !in_search()) batch_rename(selected_paths());
          else if (focus >= 0 && !in_network()) start_rename(focus);
          return 1;
        case ' ':
          if (focus >= 0 && ctrl) { if (sel.count(focus)) sel.erase(focus); else sel.insert(focus); redraw(); return 1; }
          // Space: quick look (unless a name with a space is being typed)
          if (focus >= 0 && !(st & FL_ALT) && (type_ahead.empty() || time(nullptr) - g_type_ahead_time > 1)) {
            quick_look(entry(focus).path);
            return 1;
          }
          break;
      }
      // type-ahead: jump to the first item starting with the typed text
      if (!ctrl && !(st & FL_ALT) && Fl::event_length() > 0 && (unsigned char)Fl::event_text()[0] >= ' ') {
        time_t t = time(nullptr);
        if (t - g_type_ahead_time > 1) type_ahead.clear();
        g_type_ahead_time = t;
        type_ahead += Fl::event_text();
        std::string ta = fld::lower(type_ahead);
        for (int i = 0; i < (int)items.size(); i++)
          if (fld::starts_with(fld::lower(entry(i).name), ta)) { select_only(i); break; }
        return 1;
      }
      return 0;
    }
  }
  return Fl_Group::handle(e);
}
