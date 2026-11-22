// fl-disks: disks and partitions. Shows the disks with lsblk (no root), and
// changes them through pkexec fl-disks-helper (parted, mkfs.*, resize
// tools). Every change that loses data asks first.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Table_Row.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>
#include <memory>
#include <thread>
#include <unistd.h>

#include "disks.h"
#include "fld/file_dialog.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "layout_bar.h"
#ifdef FLD_HAVE_UDISKS
#include "udclient.h"
#endif

#ifndef FLD_LIBEXECDIR
#define FLD_LIBEXECDIR "/usr/libexec/fldesktop"
#endif

using namespace disks;

namespace {

// ---- small helpers ---------------------------------------------------------------

// runs fn on the FLTK thread
void post(std::function<void()> fn) {
  Fl::awake([](void *p) {
    std::unique_ptr<std::function<void()>> f((std::function<void()> *)p);
    (*f)();
  }, new std::function<void()>(std::move(fn)));
}

std::string helper_path() {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) {
    buf[n] = 0;
    std::string local = fld::dirname(buf) + "/fl-disks-helper";
    if (fld::file_exists(local) && !fld::starts_with(local, "/usr/")) return local;
  }
  return std::string(FLD_LIBEXECDIR) + "/fl-disks-helper";
}

std::string ini_path() { return fld::fld_config_dir() + "/disks.ini"; }

bool show_loop_devices() {
  const char *e = getenv("FLD_DISKS_SHOW_LOOP");
  return e && !strcmp(e, "1");
}

std::string mib(uint64_t b) { return std::to_string(b / kMiB); }

void run_modal(Fl_Double_Window *w, const bool *done) {
  w->set_modal();
  w->show();
  while (w->shown() && !*done) Fl::wait();
  w->hide();
}

Fl_Box *add_label(int x, int y, int w, int h, const std::string &t, Fl_Align a = FL_ALIGN_RIGHT) {
  Fl_Box *b = new Fl_Box(x, y, w, h);
  b->copy_label(fld::escape_at(t).c_str());
  b->align(FL_ALIGN_INSIDE | a | FL_ALIGN_WRAP);
  return b;
}

Fl_Button *danger_button(int x, int y, int w, int h, const char *l) {
  Fl_Button *b = new Fl_Button(x, y, w, h, l);
  b->color(fld::palette().danger);
  b->labelcolor(FL_WHITE);
  return b;
}

// A dialog with a gray "Cancel" and an action button; the action button can
// wait for an "I understand" check box.
struct Dialog {
  Fl_Double_Window *win;
  Fl_Button *ok = nullptr;
  Fl_Check_Button *ack = nullptr;
  bool done = false, accepted = false;
  std::function<bool()> valid;  // more conditions for the action button
  int y = 16;
  const int W;

  Dialog(int w, int h, const std::string &title) : W(w) {
    win = new Fl_Double_Window(w, h);
    win->copy_label(title.c_str());
  }
  ~Dialog() { delete win; }

  Fl_Box *text(const std::string &t, int lines, bool bold = false) {
    Fl_Box *b = add_label(20, y, W - 40, 18 * lines + 4, t, FL_ALIGN_LEFT);
    b->align(b->align() | FL_ALIGN_TOP);
    if (bold) b->labelfont(FL_HELVETICA_BOLD);
    y += 18 * lines + 10;
    return b;
  }
  // a "label: widget" row; returns the widget's x and width
  void row(const char *label, int h, int &wx, int &ww) {
    add_label(20, y, 150, h, label);
    wx = 180;
    ww = W - 200;
  }
  void next(int h) { y += h + 8; }
  void acknowledge(const std::string &t) {
    ack = new Fl_Check_Button(20, y, W - 40, 26);
    ack->copy_label(fld::escape_at(t).c_str());
    ack->labelfont(FL_HELVETICA_BOLD);
    ack->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->update(); }, this);
    y += 34;
  }
  // adds the buttons below the rows and fits the window to them
  void buttons(const char *action, bool dangerous) {
    int by = y + 12;
    win->size(W, by + 46);
    Fl_Button *cancel = new Fl_Button(W - 280, by, 110, 30, "Cancel");
    cancel->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->done = true; }, this);
    ok = dangerous ? danger_button(W - 160, by, 140, 30, action) : new Fl_Return_Button(W - 160, by, 140, 30, action);
    ok->callback([](Fl_Widget *, void *d) {
      Dialog *g = (Dialog *)d;
      g->accepted = g->done = true;
    }, this);
    win->end();
  }
  void update() {
    if (!ok) return;
    bool good = (!ack || ack->value()) && (!valid || valid());
    good ? ok->activate() : ok->deactivate();
  }
  bool run() {
    update();
    run_modal(win, &done);
    return accepted;
  }
};

void changed_cb(Fl_Widget *, void *d) { ((Dialog *)d)->update(); }

// ---- the warning ------------------------------------------------------------------

bool startup_warning() {
  fld::Ini ini;
  ini.load(ini_path());
  if (ini.get_bool("General", "HideWarning", false)) return true;
  Dialog d(540, 320, "Disks: Be Careful");
  Fl_Box *icon = new Fl_Box(20, 18, 48, 48);
  if (Fl_Image *im = fld::icon("dialog-warning", 48)) icon->image(im);
  d.y = 18;
  Fl_Box *h = add_label(80, d.y, 440, 26, "Disks is an advanced tool", FL_ALIGN_LEFT);
  h->labelfont(FL_HELVETICA_BOLD);
  h->labelsize(16);
  Fl_Box *t = add_label(80, d.y + 30, 440, 160,
                        "Creating, deleting, resizing or formatting partitions can erase all the data on a drive, and a "
                        "mistake with the drive the system runs from can leave the computer unable to start.\n\n"
                        "Back up your files before you change anything. Every change asks once more before it is "
                        "made, and needs an administrator password.",
                        FL_ALIGN_LEFT);
  t->align(t->align() | FL_ALIGN_TOP);
  Fl_Check_Button *hide = new Fl_Check_Button(80, 222, 440, 26, "Don't show this again");
  int by = d.win->h() - 46;
  Fl_Button *quit = new Fl_Button(d.W - 280, by, 110, 30, "Quit");
  quit->callback([](Fl_Widget *, void *p) { ((Dialog *)p)->done = true; }, &d);
  d.ok = new Fl_Return_Button(d.W - 160, by, 140, 30, "I Understand");
  d.ok->callback([](Fl_Widget *, void *p) { ((Dialog *)p)->accepted = ((Dialog *)p)->done = true; }, &d);
  d.win->end();
  bool ok = d.run();
  if (ok && hide->value()) {
    ini.set_bool("General", "HideWarning", true);
    ini.save(ini_path());
  }
  return ok;
}

// ---- the window --------------------------------------------------------------------

struct Item {
  bool free = false;
  int index = 0;  // into partitions or free
  uint64_t start = 0, size = 0;
  bool inner = false;
};

class PartTable;

struct App {
  std::vector<Disk> disks;
  std::string last_json;
  std::string disk_name;           // selected disk
  std::vector<Item> items;
  int item = -1;                   // selected item
  bool busy = false;
  Fl_Double_Window *win = nullptr;
  Fl_Hold_Browser *list = nullptr;
  Fl_Box *title = nullptr, *info = nullptr, *health = nullptr, *status = nullptr, *details = nullptr;
  LayoutBar *bar = nullptr;
  PartTable *table = nullptr;
  Fl_Progress *progress = nullptr;
  fld::FlatButton *b_table = nullptr, *b_backup = nullptr, *b_restore = nullptr;
  Fl_Button *b_new = nullptr, *b_delete = nullptr, *b_format = nullptr, *b_resize = nullptr, *b_edit = nullptr,
            *b_mount = nullptr;

  const Disk *disk() const {
    for (auto &d : disks)
      if (d.name == disk_name) return &d;
    return nullptr;
  }
  const Partition *part() const {
    const Disk *d = disk();
    if (!d || item < 0 || item >= (int)items.size() || items[item].free) return nullptr;
    return &d->partitions[items[item].index];
  }
  const Region *free_region() const {
    const Disk *d = disk();
    if (!d || item < 0 || item >= (int)items.size() || !items[item].free) return nullptr;
    return &d->free[items[item].index];
  }
} A;

void show_disk();
void update_buttons();
void refresh(bool force);

const char *kCols[] = {"Partition", "Name", "File system", "Label", "Size", "Used", "Flags", "Mounted at"};
const int kNumCols = 8;

std::string fs_title(const std::string &fs) {
  if (fs.empty()) return "";
  if (const FsInfo *f = fs_info(fs)) {
    std::string t = f->title;
    return t.substr(0, t.find(" ("));
  }
  if (fs == "crypto_LUKS") return "Encrypted (LUKS)";
  if (fs == "LVM2_member") return "LVM";
  if (fs == "linux_raid_member") return "RAID";
  return fs;
}

class PartTable : public Fl_Table_Row {
public:
  PartTable(int x, int y, int w, int h) : Fl_Table_Row(x, y, w, h) {
    type(SELECT_SINGLE);
    cols(kNumCols);
    col_header(1);
    col_header_height(26);
    row_height_all(24);
    col_resize(1);
    const int widths[] = {100, 100, 90, 80, 70, 70, 90, 100};
    for (int i = 0; i < kNumCols; i++) col_width(i, widths[i]);
    color(fld::palette().bg2);
    selection_color(fld::palette().accent);
    end();
    callback([](Fl_Widget *w, void *) {
      PartTable *t = (PartTable *)w;
      if (t->callback_context() == CONTEXT_CELL) {
        A.item = t->callback_row();
        A.bar->select_item(A.item);
        update_buttons();
      }
    });
    when(FL_WHEN_RELEASE | FL_WHEN_CHANGED);
  }
  void reload() {
    rows((int)A.items.size());
    select_all_rows(0);
    if (A.item >= 0 && A.item < rows()) select_row(A.item, 1);
    redraw();
  }
  std::string cell(int R, int C) const {
    const Disk *d = A.disk();
    if (!d || R < 0 || R >= (int)A.items.size()) return "";
    const Item &it = A.items[R];
    if (it.free) {
      if (C == 0) return std::string(it.inner ? "    " : "") + "Free space";
      if (C == 4) return size_text(it.size);
      return "";
    }
    const Partition &p = d->partitions[it.index];
    switch (C) {
      case 0: return std::string(p.logical ? "    " : "") + fld::basename(p.device);
      case 1: return d->table == "gpt" ? p.name : p.extended ? "extended" : p.logical ? "logical" : "primary";
      case 2: return fs_title(p.fs_type);
      case 3: return p.label;
      case 4: return size_text(p.size);
      case 5: return p.fs_used >= 0 ? size_text((uint64_t)p.fs_used) : "";
      case 6: return fld::join(current_flags(p, d->table), ", ");
      default: return fld::join(p.mount_points, ", ");
    }
  }

protected:
  void draw_cell(TableContext ctx, int R, int C, int X, int Y, int W, int H) override {
    const fld::Palette &p = fld::palette();
    switch (ctx) {
      case CONTEXT_COL_HEADER:
        fl_push_clip(X, Y, W, H);
        fl_draw_box(FL_FLAT_BOX, X, Y, W, H, p.bg);
        fl_color(p.border);
        fl_line(X, Y + H - 1, X + W, Y + H - 1);
        fl_color(p.fg);
        fl_font(FL_HELVETICA_BOLD, 13);
        fl_draw(kCols[C], X + 6, Y, W - 12, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
        fl_pop_clip();
        return;
      case CONTEXT_CELL: {
        fl_push_clip(X, Y, W, H);
        bool sel = row_selected(R);
        fl_color(sel ? p.accent : p.bg2);
        fl_rectf(X, Y, W, H);
        std::string s = fld::escape_at(cell(R, C));
        bool free = R < (int)A.items.size() && A.items[R].free;
        if (C == 0 && !free && A.disk()) {
          const Partition &pt = A.disk()->partitions[A.items[R].index];
          fl_color(LayoutBar::fs_color(pt.extended ? "extended" : fs_id_from_lsblk(pt.fs_type).empty()
                                                                       ? pt.fs_type
                                                                       : fs_id_from_lsblk(pt.fs_type)));
          int ind = pt.logical ? 16 : 0;
          fl_rectf(X + 6 + ind, Y + 7, 10, 10);
          s = fld::escape_at(fld::basename(pt.device));
          fl_color(sel ? p.accent_fg : p.fg);
          fl_font(FL_HELVETICA, 13);
          fl_draw(s.c_str(), X + 22 + ind, Y, W - 28 - ind, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
        } else {
          fl_color(sel ? p.accent_fg : free ? p.fg_dim : p.fg);
          fl_font(free ? FL_HELVETICA_ITALIC : FL_HELVETICA, 13);
          fl_draw(s.c_str(), X + 6, Y, W - 12, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
        }
        fl_pop_clip();
        return;
      }
      default:
        return;
    }
  }
};

void set_status(const std::string &s) {
  A.status->copy_label(fld::escape_at(s).c_str());
  A.status->redraw();
}

// ---- running the helper ----------------------------------------------------------

// Runs pkexec fl-disks-helper args... in a thread with progress, then
// refreshes and calls done (FLTK thread) with the error ("" = success).
void run_helper(const std::vector<std::string> &args, const std::string &doing, const std::string &done_text,
                std::function<void(const std::string &)> done = nullptr) {
  if (A.busy) return;
  if (fld::which("pkexec").empty()) {
    fl_alert("pkexec (polkit) is needed to change disks.");
    return;
  }
  A.busy = true;
  update_buttons();
  set_status(doing + " (polkit asks for an administrator password)");
  A.progress->value(0);
  A.progress->show();
  std::vector<std::string> cmd{"pkexec", helper_path()};
  cmd.insert(cmd.end(), args.begin(), args.end());
  std::thread([cmd, done, done_text]() {
    std::vector<std::string> lines;
    int rc = fld::run_lines(cmd, [&](const std::string &line) {
      uint64_t a, b;
      if (parse_progress(line, a, b)) {
        post([a, b] {
          A.progress->maximum(1);
          A.progress->value(b ? (float)((double)a / (double)b) : 0.f);
          char t[96];
          snprintf(t, sizeof t, "%s of %s", size_text(a).c_str(), size_text(b).c_str());
          A.progress->copy_label(t);
        });
      } else if (fld::starts_with(line, "STEP ")) {
        std::string s = line.substr(5) + "…";
        post([s] { set_status(s); });
      } else if (!fld::starts_with(line, "PARTITION ")) {
        lines.push_back(line);
      }
    });
    std::string out = fld::trim(fld::join(lines, "\n"));
    post([rc, out, done, done_text] {
      A.busy = false;
      A.progress->hide();
      A.progress->label(nullptr);
      std::string err;
      if (rc == 126 || rc == 127) err = "The authentication was cancelled or failed.";
      else if (rc != 0) err = out.empty() ? "fl-disks-helper failed." : out;
      if (err.empty()) set_status(done_text);
      else set_status("Not done: " + err);
      refresh(true);
      if (!err.empty() && rc != 126 && rc != 127) fl_alert("%s", err.c_str());
      if (done) done(err);
    });
  }).detach();
}

// ---- dialogs for the actions -------------------------------------------------------

std::string disk_words(const Disk &d) { return d.title() + " (" + d.device + ", " + size_text(d.size) + ")"; }

std::string part_words(const Partition &p) {
  std::string s = p.device;
  if (!p.label.empty()) s += " \"" + p.label + "\"";
  s += ", " + size_text(p.size);
  if (!p.fs_type.empty()) s += ", " + fs_title(p.fs_type);
  return s;
}

// fills a file system choice; returns the ids in menu order
std::vector<std::string> fill_fs(Fl_Choice *c, bool with_none, const std::string &select) {
  std::vector<std::string> ids;
  int sel = -1;
  for (auto &f : filesystems()) {
    bool have = !fld::which(f.tool).empty() || fld::file_exists(std::string("/usr/sbin/") + f.tool) ||
                fld::file_exists(std::string("/sbin/") + f.tool);
    std::string l = std::string(f.title) + (have ? "" : std::string("  — needs ") + f.package);
    for (auto ch : {'/', '\\', '&', '_'})
      for (size_t i = 0; (i = l.find(ch, i)) != std::string::npos; i += 2) l.insert(i, "\\");
    c->add(l.c_str(), 0, nullptr, nullptr, have ? 0 : FL_MENU_INACTIVE);
    ids.push_back(f.id);
    if (have && (sel < 0 || f.id == select)) sel = (int)ids.size() - 1;
  }
  if (with_none) {
    c->add("Leave unformatted");
    ids.push_back("none");
  }
  c->value(sel < 0 ? (int)ids.size() - 1 : sel);
  return ids;
}

void new_table() {
  const Disk *d = A.disk();
  if (!d) return;
  Dialog g(540, 300, "New Partition Table");
  g.text("Create a new partition table on " + disk_words(*d) + "?", 2, true);
  g.text("All the partitions on this disk and the data on them will be lost.", 1);
  int wx, ww;
  g.row("Type:", 28, wx, ww);
  Fl_Choice *type = new Fl_Choice(wx, g.y, 260, 28);
  type->add("GPT (recommended)");
  type->add("MBR (DOS, for old computers)");
  type->value(0);
  g.next(28);
  g.acknowledge("I understand that all data on this disk will be erased");
  g.buttons("Erase Disk", true);
  if (!g.run()) return;
  std::string dev = d->device;
  run_helper({"mklabel", dev, type->value() == 0 ? "gpt" : "msdos"}, "Creating the partition table…",
             "Created a new partition table on " + dev + ".");
}

void new_partition() {
  const Disk *d = A.disk();
  const Region *r = A.free_region();
  if (!d || !r) return;
  Region region = *r;
  Dialog g(540, 380, "New Partition");
  g.text("In " + size_text(region.size) + " of free space on " + disk_words(*d) + ".", 2);
  int wx, ww;
  g.row("Size:", 28, wx, ww);
  Fl_Input *size = new Fl_Input(wx, g.y, 120, 28);
  size->value(mib(region.size).c_str());
  size->when(FL_WHEN_CHANGED);
  size->callback(changed_cb, &g);
  add_label(wx + 130, g.y, ww - 130, 28, "MiB (up to " + mib(region.size) + "; or e.g. \"20 GiB\")", FL_ALIGN_LEFT)
      ->labelsize(12);
  g.next(28);
  g.row("Place:", 28, wx, ww);
  Fl_Choice *place = new Fl_Choice(wx, g.y, 240, 28);
  place->add("At the start of the free space");
  place->add("At the end of the free space");
  place->value(0);
  g.next(28);
  g.row("File system:", 28, wx, ww);
  Fl_Choice *fs = new Fl_Choice(wx, g.y, ww, 28);
  std::vector<std::string> ids = fill_fs(fs, true, "ext4");
  g.next(28);
  Fl_Input *name = nullptr;
  if (d->table == "gpt") {
    g.row("Partition name:", 28, wx, ww);
    name = new Fl_Input(wx, g.y, ww, 28);
    name->tooltip("Optional. The GPT name of the partition (not the file system label)");
    g.next(28);
  }
  g.row("Label:", 28, wx, ww);
  Fl_Input *label = new Fl_Input(wx, g.y, ww, 28);
  label->tooltip("Optional. The name the file system shows in Files");
  g.next(28);
  Fl_Box *msg = add_label(20, g.y, g.W - 40, 22, "", FL_ALIGN_LEFT);
  msg->labelsize(12);
  msg->labelcolor(fld::palette().danger);
  std::string msg_text;
  g.valid = [&]() {
    uint64_t s = align_down(parse_size(size->value()));
    std::string err;
    if (s < kMiB) err = "Enter a size of at least 1 MiB.";
    else if (s > region.size) err = "That is more than the free space (" + mib(region.size) + " MiB).";
    else if (name && !valid_partition_name(name->value(), err)) {
    }
    msg_text = err;
    msg->label(msg_text.c_str());
    msg->redraw();
    return err.empty();
  };
  if (name) {
    name->when(FL_WHEN_CHANGED);
    name->callback(changed_cb, &g);
  }
  g.buttons("Create", false);
  if (!g.run()) return;
  uint64_t s = align_down(parse_size(size->value()));
  uint64_t start = place->value() == 0 ? region.start : region.end() - s;
  std::string f = ids[std::max(0, fs->value())];
  std::string nm = name && *name->value() ? name->value() : "-";
  std::string lb = *label->value() ? label->value() : "-";
  run_helper({"mkpart", d->device, std::to_string(start), std::to_string(start + s), f, nm, lb},
             "Creating the partition…", "Created a " + size_text(s) + " partition.");
}

void delete_partition() {
  const Disk *d = A.disk();
  const Partition *p = A.part();
  if (!d || !p) return;
  Dialog g(540, 250, "Delete Partition");
  g.text("Delete " + part_words(*p) + " on " + d->title() + "?", 2, true);
  g.text("The partition and all the files on it will be lost.", 1);
  g.acknowledge("I understand that the data on this partition will be lost");
  g.buttons("Delete", true);
  if (!g.run()) return;
  run_helper({"delete", d->device, std::to_string(p->number)}, "Deleting " + p->device + "…",
             "Deleted " + p->device + ".");
}

void format_partition() {
  const Disk *d = A.disk();
  const Partition *p = A.part();
  if (!d || !p) return;
  Dialog g(540, 320, "Format Partition");
  g.text("Format " + part_words(*p) + "?", 2, true);
  g.text("A new, empty file system replaces the one there: all its files will be lost.", 1);
  int wx, ww;
  g.row("File system:", 28, wx, ww);
  Fl_Choice *fs = new Fl_Choice(wx, g.y, ww, 28);
  std::string cur = fs_id_from_lsblk(p->fs_type);
  std::vector<std::string> ids = fill_fs(fs, false, cur.empty() ? "ext4" : cur);
  g.next(28);
  g.row("Label:", 28, wx, ww);
  Fl_Input *label = new Fl_Input(wx, g.y, ww, 28);
  label->value(p->label.c_str());
  g.next(28);
  g.acknowledge("I understand that the data on this partition will be lost");
  g.buttons("Format", true);
  if (!g.run()) return;
  std::string f = ids[std::max(0, fs->value())];
  run_helper({"format", d->device, std::to_string(p->number), f, *label->value() ? label->value() : "-"},
             "Formatting " + p->device + "…", "Formatted " + p->device + ".");
}

void resize_partition() {
  const Disk *d = A.disk();
  const Partition *p = A.part();
  if (!d || !p) return;
  Partition part = *p;
  uint64_t maxs = max_size(*d, part);
  std::string fs = fs_id_from_lsblk(part.fs_type);
  const FsInfo *fi = fs_info(fs);
  bool movable = part.fs_type.empty() && !part.logical;
  Dialog g(560, movable ? 420 : 380, "Resize Partition");
  g.text("Resize " + part_words(part) + ".", 2, true);
  std::string can;
  if (part.fs_type.empty()) can = "It has no file system: it can grow, shrink and move.";
  else if (!fi) can = "Its file system (" + part.fs_type + ") cannot be resized here.";
  else if (fi->grow && fi->shrink) can = std::string(fi->title) + " can grow and shrink.";
  else if (fi->grow) can = std::string(fi->title) + " can only grow.";
  else can = std::string(fi->title) + " cannot be resized.";
  g.text(can + " A partition with files can only move by backing it up, deleting it and creating it again.", 3);
  int wx, ww;
  g.row("New size:", 28, wx, ww);
  Fl_Input *size = new Fl_Input(wx, g.y, 120, 28);
  size->value(mib(part.size).c_str());
  size->when(FL_WHEN_CHANGED);
  size->callback(changed_cb, &g);
  add_label(wx + 130, g.y, ww - 130, 28, "MiB (now " + mib(part.size) + ", up to " + mib(maxs) + ")", FL_ALIGN_LEFT)
      ->labelsize(12);
  g.next(28);
  Fl_Input *start = nullptr;
  if (movable) {
    g.row("Start at:", 28, wx, ww);
    start = new Fl_Input(wx, g.y, 120, 28);
    start->value(mib(part.start).c_str());
    start->when(FL_WHEN_CHANGED);
    start->callback(changed_cb, &g);
    add_label(wx + 130, g.y, ww - 130, 28, "MiB from the start of the disk", FL_ALIGN_LEFT)->labelsize(12);
    g.next(28);
  }
  Fl_Box *msg = add_label(20, g.y, g.W - 40, 36, "", FL_ALIGN_LEFT);
  msg->labelsize(12);
  msg->labelcolor(fld::palette().danger);
  g.y += 40;
  g.acknowledge("I have a backup of the files on this partition");
  std::string msg_text;
  const Disk disk = *d;
  g.valid = [&]() {
    uint64_t s = align_down(parse_size(size->value()));
    uint64_t st = start ? align_down(parse_size(start->value())) : part.start;
    std::string err;
    std::vector<Step> steps;
    if (s < kMiB) err = "Enter a size of at least 1 MiB.";
    else if (st != part.start) {
      // moving: the new place has to be in this partition's space or free space next to it
      uint64_t lo = part.start, hi = part.end();
      for (auto &r : disk.free) {
        if (r.end() >= lo && r.start <= lo) lo = r.start;
        if (r.start <= hi && r.end() >= hi) hi = r.end();
      }
      if (st < lo || st + s > hi) err = "There is no free space there.";
    } else if (s > maxs) err = "That is more than there is room for (" + mib(maxs) + " MiB).";
    else if (s == part.size) err = "";
    else if (!resize_plan(disk.device, part.number, part.device, part.fs_type, part.start, part.size, s, steps, err)) {
    }
    msg_text = err;
    msg->label(msg_text.c_str());
    msg->redraw();
    bool same = s == part.size && st == part.start;
    return err.empty() && !same;
  };
  g.buttons("Resize", true);
  if (!g.run()) return;
  uint64_t s = align_down(parse_size(size->value()));
  uint64_t st = start ? align_down(parse_size(start->value())) : part.start;
  std::string dev = d->device, num = std::to_string(part.number);
  if (st != part.start) {
    // move first (same size), then resize if asked
    bool also = s != part.size;
    run_helper({"move", dev, num, std::to_string(st)}, "Moving " + part.device + "…", "Moved " + part.device + ".",
               [dev, num, s, also, part](const std::string &err) {
                 if (err.empty() && also)
                   run_helper({"resize", dev, num, std::to_string(s)}, "Resizing " + part.device + "…",
                              "Moved and resized " + part.device + ".");
               });
    return;
  }
  run_helper({"resize", dev, num, std::to_string(s)}, "Resizing " + part.device + "…",
             "Resized " + part.device + " to " + size_text(s) + ".");
}

void edit_partition() {
  const Disk *d = A.disk();
  const Partition *p = A.part();
  if (!d || !p) return;
  Partition part = *p;
  bool gpt = d->table == "gpt";
  Dialog g(500, gpt ? 400 : 350, "Edit Partition");
  g.text("Partition " + part_words(part), 2, true);
  int wx, ww;
  Fl_Input *name = nullptr;
  if (gpt) {
    g.row("Name:", 28, wx, ww);
    name = new Fl_Input(wx, g.y, ww, 28);
    name->value(part.name.c_str());
    name->when(FL_WHEN_CHANGED);
    name->callback(changed_cb, &g);
    g.next(28);
  }
  g.row("Flags:", 170, wx, ww);
  Fl_Check_Browser *flags = new Fl_Check_Browser(wx, g.y, ww, 170);
  std::vector<std::string> all = partition_flags(d->table), cur = current_flags(part, d->table);
  for (auto &f : all) flags->add(f.c_str(), std::find(cur.begin(), cur.end(), f) != cur.end());
  flags->when(FL_WHEN_CHANGED);
  flags->callback(changed_cb, &g);
  g.next(170);
  g.text("esp marks the EFI system partition, bios_grub the GRUB partition of BIOS computers, boot the "
         "partition an old BIOS starts from.",
         3)
      ->labelsize(12);
  std::string err_text;
  g.valid = [&]() {
    std::string err;
    return !name || valid_partition_name(name->value(), err);
  };
  g.buttons("Apply", false);
  if (!g.run()) return;
  std::string dev = d->device, num = std::to_string(part.number);
  // one helper call per change: chained, polkit asks once
  auto jobs = std::make_shared<std::vector<std::vector<std::string>>>();
  if (name && part.name != name->value())
    jobs->push_back({"name", dev, num, *name->value() ? name->value() : "-"});
  for (int i = 1; i <= flags->nitems(); i++) {
    bool on = flags->checked(i);
    bool was = std::find(cur.begin(), cur.end(), all[i - 1]) != cur.end();
    if (on != was) jobs->push_back({"flag", dev, num, all[i - 1], on ? "on" : "off"});
  }
  if (jobs->empty()) return;
  auto next = std::make_shared<std::function<void(size_t)>>();
  *next = [jobs, next, part](size_t i) {
    if (i >= jobs->size()) return;
    run_helper((*jobs)[i], "Changing " + part.device + "…", "Changed " + part.device + ".",
               [next, i](const std::string &err) {
                 if (err.empty()) (*next)(i + 1);
               });
  };
  (*next)(0);
}

void mount_toggle() {
  const Partition *p = A.part();
  if (!p || A.busy) return;
  std::string dev = p->device;
  bool mounted = p->mounted();
  if (fld::which("udisksctl").empty()) {
    fl_alert("udisksctl (udisks2) is needed to mount partitions.");
    return;
  }
  A.busy = true;
  update_buttons();
  set_status((mounted ? "Unmounting " : "Mounting ") + dev + "…");
  std::thread([dev, mounted]() {
    std::string out;
    int rc = fld::run_capture_input({"udisksctl", mounted ? "unmount" : "mount", "-b", dev}, "", out, true);
    post([rc, out, dev, mounted] {
      A.busy = false;
      set_status(rc == 0 ? fld::trim(out) : "Not done: " + fld::trim(out));
      refresh(true);
    });
  }).detach();
}

void backup_image() {
  const Disk *d = A.disk();
  if (!d) return;
  fld::FileDialogOptions o;
  o.mode = fld::FileDialogOptions::SAVE;
  o.title = "Back Up " + d->title() + " to an Image";
  char date[32];
  time_t now = time(nullptr);
  strftime(date, sizeof date, "%Y-%m-%d", localtime(&now));
  std::string base = d->model.empty() ? d->name : d->model;
  for (auto &c : base)
    if (c == ' ' || c == '/') c = '-';
  o.name = base + "-" + date + ".img";
  o.filters.push_back({"Disk images", {"*.img"}, {}});
  fld::FileDialogResult r = fld::run_file_dialog(o);
  if (!r.ok || r.paths.empty()) return;
  std::string path = r.paths[0];
  if (d->any_mounted() &&
      fl_choice("Some partitions of %s are mounted. Files that change while it is copied can be damaged in the image.\n\n"
                "Unmount them first for a clean copy.",
                "Cancel", "Copy Anyway", nullptr, d->device.c_str()) != 1)
    return;
  run_helper({"image-read", d->device, path}, "Copying " + d->device + "…",
             "Saved " + d->device + " to " + fld::tilde(path) + ".");
}

void restore_image() {
  const Disk *d = A.disk();
  if (!d) return;
  fld::FileDialogOptions o;
  o.title = "Write an Image to " + d->title();
  o.filters.push_back({"Disk images", {"*.img", "*.iso", "*.raw", "*.bin"}, {}});
  o.filters.push_back({"All files", {"*"}, {}});
  fld::FileDialogResult r = fld::run_file_dialog(o);
  if (!r.ok || r.paths.empty()) return;
  std::string path = r.paths[0];
  Dialog g(560, 270, "Write Image");
  g.text("Write " + fld::basename(path) + " to " + disk_words(*d) + "?", 2, true);
  g.text("Everything on the disk is replaced by the image: all its partitions and their data will be lost.", 2);
  g.acknowledge("I understand that all data on this disk will be erased");
  g.buttons("Write", true);
  if (!g.run()) return;
  run_helper({"image-write", path, d->device}, "Writing the image…", "Wrote " + fld::basename(path) + " to " + d->device + ".");
}

// ---- showing ---------------------------------------------------------------------

void update_buttons() {
  const Disk *d = A.disk();
  const Partition *p = A.part();
  const Region *r = A.free_region();
  bool idle = !A.busy;
  auto on = [](Fl_Widget *w, bool b) { b ? w->activate() : w->deactivate(); };
  bool writable = d && !d->read_only;
  on(A.b_table, idle && writable && !d->any_mounted());
  on(A.b_backup, idle && d);
  on(A.b_restore, idle && writable && !d->any_mounted());
  on(A.b_new, idle && writable && r);
  bool part_free = p && !p->mounted() && writable;
  on(A.b_delete, idle && part_free);
  on(A.b_format, idle && part_free && !p->extended);
  on(A.b_resize, idle && part_free && !p->extended);
  on(A.b_edit, idle && p && writable && !p->extended);
  bool mountable = p && !p->extended && !p->fs_type.empty() && p->fs_type != "swap" && p->fs_type != "crypto_LUKS" &&
                   p->fs_type != "LVM2_member" && p->fs_type != "linux_raid_member";
  on(A.b_mount, idle && mountable);
  A.b_mount->label(p && p->mounted() ? "Unmount" : "Mount");
  std::string det;
  if (p) {
    det = p->device + " — starts at " + mib(p->start) + " MiB, " + mib(p->size) + " MiB";
    if (!p->type_name.empty()) det += " — " + p->type_name;
    if (!p->uuid.empty()) det += " — UUID " + p->uuid;
    if (p->mounted()) det += "  (mounted: unmount it to change it)";
  } else if (r) {
    det = "Free space — from " + mib(r->start) + " MiB, " + mib(r->size) + " MiB";
  } else if (d && d->table.empty()) {
    det = d->fs_type.empty() ? "This disk has no partition table. Create one to add partitions."
                             : "The whole disk holds a " + fs_title(d->fs_type) + " file system, without partitions.";
  }
  A.details->copy_label(fld::escape_at(det).c_str());
  A.win->redraw();
}

void show_disk() {
  const Disk *d = A.disk();
  A.items.clear();
  std::vector<LayoutBar::Seg> segs;
  if (!d) {
    A.title->label("No disk");
    A.info->label("");
    A.health->hide();
    A.bar->set({});
    A.table->reload();
    update_buttons();
    return;
  }
  A.title->copy_label(fld::escape_at(d->title()).c_str());
  std::string inf = d->device + " · " + size_text(d->size) + " · partition table: " + d->table_name();
  if (!d->transport.empty())
    inf += " · " + (fld::lower(d->transport) == "nvme" ? std::string("NVMe") : fld::lower(d->transport) == "usb" ? std::string("USB") : d->transport);
  if (d->removable) inf += " · removable";
  if (d->read_only) inf += " · read-only";
  if (!d->serial.empty()) inf += " · serial " + d->serial;
  A.info->copy_label(fld::escape_at(inf).c_str());
  A.health->hide();
#ifdef FLD_HAVE_UDISKS
  if (const ud::Drive *dr = ud::Client::get().state().drive_for_device(d->device)) {
    std::string h = ud::health_text(*dr);
    if (!h.empty()) {
      A.health->copy_label(fld::escape_at("Health: " + h).c_str());
      bool bad = !dr->healthy();
      A.health->box(bad ? FL_FLAT_BOX : FL_NO_BOX);
      A.health->color(fld::palette().danger);
      A.health->labelcolor(bad ? FL_WHITE : fld::palette().fg_dim);
      A.health->labelfont(bad ? FL_HELVETICA_BOLD : FL_HELVETICA);
      A.health->show();
    }
  }
#endif
  // partitions and free space in disk order (logical ones after their extended one)
  struct E {
    uint64_t start;
    bool inner;
    Item it;
  };
  std::vector<E> es;
  for (size_t i = 0; i < d->partitions.size(); i++) {
    const Partition &p = d->partitions[i];
    es.push_back({p.start, p.logical, {false, (int)i, p.start, p.size, p.logical}});
  }
  for (size_t i = 0; i < d->free.size(); i++)
    es.push_back({d->free[i].start, d->free[i].logical, {true, (int)i, d->free[i].start, d->free[i].size, d->free[i].logical}});
  std::sort(es.begin(), es.end(), [](const E &a, const E &b) { return a.start < b.start; });
  for (auto &e : es) A.items.push_back(e.it);
  for (size_t i = 0; i < A.items.size(); i++) {
    const Item &it = A.items[i];
    LayoutBar::Seg s;
    s.start = it.start;
    s.size = it.size;
    s.inner = it.inner;
    s.item = (int)i;
    s.sub = size_text(it.size);
    if (it.free) {
      s.fs = "free";
      s.label = "Free";
    } else {
      const Partition &p = d->partitions[it.index];
      std::string id = fs_id_from_lsblk(p.fs_type);
      s.fs = id.empty() ? p.fs_type : id;
      s.container = p.extended;
      s.label = !p.label.empty() ? p.label : !p.name.empty() ? p.name : fld::basename(p.device);
      if (!p.fs_type.empty()) s.sub += " " + fs_title(p.fs_type);
      if (p.fs_used >= 0 && p.fs_size > 0) s.used = (double)p.fs_used / (double)p.fs_size;
    }
    segs.push_back(s);
  }
  if (A.item >= (int)A.items.size()) A.item = -1;
  A.bar->set(segs);
  A.bar->select_item(A.item);
  A.table->reload();
  update_buttons();
}

void fill_list() {
  A.list->clear();
  int sel = 0;
  for (size_t i = 0; i < A.disks.size(); i++) {
    const Disk &d = A.disks[i];
    A.list->add(fld::escape_at(d.title()).c_str());
    std::string icon = d.removable || fld::lower(d.transport) == "usb" ? "drive-removable-media"
                       : fld::starts_with(d.name, "mmcblk")              ? "media-flash"
                                                                         : "drive-harddisk";
    A.list->icon(A.list->size(), fld::icon_first({icon, "drive-harddisk"}, 32));
    if (d.name == A.disk_name) sel = (int)i + 1;
  }
  if (!sel && !A.disks.empty()) {
    sel = 1;
    A.disk_name = A.disks[0].name;
    A.item = -1;
  }
  if (sel) A.list->value(sel);
}

// the second line of each disk in the list: size and device
class DiskList : public Fl_Hold_Browser {
public:
  DiskList(int x, int y, int w, int h) : Fl_Hold_Browser(x, y, w, h) {}
  int item_height(void *it) const override { return 48; }
  void item_draw(void *it, int X, int Y, int W, int H) const override {
    int line = 0;
    for (void *i = item_first(); i; i = item_next(i)) {
      line++;
      if (i == it) break;
    }
    const fld::Palette &p = fld::palette();
    bool sel = line == value();
    if (sel) {
      fl_color(p.accent);
      fl_rectf(X, Y, W, H);
    }
    if (line < 1 || line > (int)A.disks.size()) return;
    const Disk &d = A.disks[line - 1];
    if (Fl_Image *im = icon(line)) im->draw(X + 6, Y + (H - im->h()) / 2);
    fl_color(sel ? p.accent_fg : p.fg);
    fl_font(FL_HELVETICA_BOLD, 13);
    std::string t = fld::escape_at(d.title());
    fl_draw(t.c_str(), X + 46, Y + 4, W - 50, 20, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    fl_font(FL_HELVETICA, 12);
    fl_color(sel ? p.accent_fg : p.fg_dim);
    std::string s = size_text(d.size) + " · " + d.device;
#ifdef FLD_HAVE_UDISKS
    if (const ud::Drive *dr = ud::Client::get().state().drive_for_device(d.device))
      if (dr->smart_supported && !dr->healthy()) {
        s = "⚠ " + s;
        if (!sel) fl_color(p.danger);
      }
#endif
    fl_draw(fld::escape_at(s).c_str(), X + 46, Y + 24, W - 50, 20, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  }
};

void refresh(bool force) {
  if (A.busy && !force) return;
  std::string out;
  int rc = fld::run_capture(lsblk_command(), out);
  if (rc != 0) rc = fld::run_capture(lsblk_command(true), out);  // util-linux < 2.39
  if (!force && out == A.last_json) return;
  A.last_json = out;
  std::string err;
  std::vector<Disk> ds;
  if (rc != 0 || !parse_lsblk(out, ds, show_loop_devices(), &err)) {
    set_status("Cannot list the disks (lsblk): " + (err.empty() ? fld::trim(out) : err));
    return;
  }
  // keep the selection on the same place of the same disk
  uint64_t sel_start = A.item >= 0 && A.item < (int)A.items.size() ? A.items[A.item].start : UINT64_MAX;
  A.disks = ds;
  fill_list();
  show_disk();
  if (sel_start != UINT64_MAX) {
    A.item = -1;
    for (size_t i = 0; i < A.items.size(); i++)
      if (A.items[i].start == sel_start) A.item = (int)i;
    A.bar->select_item(A.item);
    A.table->reload();
    update_buttons();
  }
}

void tick(void *) {
  refresh(false);
  Fl::repeat_timeout(3.0, tick);
}

int keys(int e) {
  if (e == FL_SHORTCUT && Fl::event_key() == FL_F + 5) {
    refresh(true);
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-disks [--list]\n  --list prints the disks and partitions and quits\n");
      return 0;
    }
    if (a == "--list") {
      std::string out;
      if (fld::run_capture(lsblk_command(), out) != 0) fld::run_capture(lsblk_command(true), out);
      std::vector<Disk> ds;
      if (!parse_lsblk(out, ds, show_loop_devices())) return 1;
      for (auto &d : ds) {
        printf("%s  %s  %s  %s\n", d.device.c_str(), size_text(d.size).c_str(), d.table_name().c_str(), d.title().c_str());
        for (auto &p : d.partitions)
          printf("  %-16s %10s  %-10s %s %s\n", p.device.c_str(), size_text(p.size).c_str(), p.fs_type.c_str(),
                 p.label.c_str(), fld::join(p.mount_points, ",").c_str());
        for (auto &r : d.free) printf("  %-16s %10s  at %s MiB\n", "(free)", size_text(r.size).c_str(), mib(r.start).c_str());
      }
      return 0;
    }
  }
  fld::init_app("fl-disks", argc, argv);
  Fl::lock();
  if (!startup_warning()) return 0;

  const fld::Palette &p = fld::palette();
  const int W = 1000, H = 660, TB = 42, ST = 26, SW = 230;
  A.win = new Fl_Double_Window(W, H, "Disks");
  fld::Toolbar *bar = new fld::Toolbar(0, 0, W, TB);
  bar->color(p.bg);
  bar->add_button("view-refresh", "Read the disks again (F5)", [](Fl_Widget *, void *) { refresh(true); }, nullptr,
                  "Refresh");
  bar->add_separator();
  A.b_table = bar->add_button("document-new", "Erase the disk and create a new, empty partition table",
                              [](Fl_Widget *, void *) { new_table(); }, nullptr, "New Partition Table…");
  A.b_backup = bar->add_button("document-save", "Copy the whole disk to an image file",
                               [](Fl_Widget *, void *) { backup_image(); }, nullptr, "Back Up to Image…");
  A.b_restore = bar->add_button("document-open", "Write an image file (.img, .iso) to the whole disk",
                                [](Fl_Widget *, void *) { restore_image(); }, nullptr, "Write Image…");
  Fl_Box *fill = new Fl_Box(bar->next_x(), 0, 10, TB);
  bar->add(fill);
  bar->resizable(fill);
  bar->add_button("help-about", "About Disks", [](Fl_Widget *, void *) {
    fld::about_dialog("Disks", "fl-disks", "Disks and partitions: create, delete, format and resize them with parted.");
  }, nullptr);
  bar->end();

  Fl_Tile *tile = new Fl_Tile(0, TB, W, H - TB - ST);
  A.list = new DiskList(0, TB, SW, H - TB - ST);
  A.list->box(FL_FLAT_BOX);
  A.list->color(p.bg);
  A.list->clear_visible_focus();
  A.list->callback([](Fl_Widget *w, void *) {
    int l = ((Fl_Hold_Browser *)w)->value();
    if (l < 1 || l > (int)A.disks.size()) return;
    if (A.disks[l - 1].name != A.disk_name) {
      A.disk_name = A.disks[l - 1].name;
      A.item = -1;
      show_disk();
    }
  });
  Fl_Group *right = new Fl_Group(SW, TB, W - SW, H - TB - ST);
  right->box(FL_FLAT_BOX);
  right->color(p.bg2);
  int X = SW + 16, RW = W - SW - 32, y = TB + 10;
  A.title = new Fl_Box(X, y, RW, 28);
  A.title->labelfont(FL_HELVETICA_BOLD);
  A.title->labelsize(18);
  A.title->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  y += 28;
  A.info = new Fl_Box(X, y, RW, 20);
  A.info->labelsize(12);
  A.info->labelcolor(p.fg_dim);
  A.info->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  y += 22;
  A.health = new Fl_Box(X, y, RW, 22);
  A.health->labelsize(12);
  A.health->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  y += 28;
  A.bar = new LayoutBar(X, y, RW, 64);
  A.bar->callback([](Fl_Widget *w, void *) {
    A.item = ((LayoutBar *)w)->selected_item();
    A.table->reload();
    update_buttons();
  });
  y += 74;
  int by = H - ST - 76;
  A.table = new PartTable(X, y, RW, by - y - 30);
  A.details = new Fl_Box(X, by - 26, RW, 22);
  A.details->labelsize(12);
  A.details->labelcolor(p.fg_dim);
  A.details->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  Fl_Group *btns = new Fl_Group(X, by, RW, 32);
  int bx = X;
  auto button = [&](const char *l, const char *tip, Fl_Callback *cb, int w) {
    Fl_Button *b = new Fl_Button(bx, by, w, 30, l);
    b->tooltip(tip);
    b->callback(cb);
    bx += w + 8;
    return b;
  };
  A.b_new = button("New Partition…", "Create a partition in the selected free space",
                   [](Fl_Widget *, void *) { new_partition(); }, 130);
  A.b_delete = button("Delete…", "Delete the selected partition", [](Fl_Widget *, void *) { delete_partition(); }, 80);
  A.b_format = button("Format…", "Make a new file system on the selected partition",
                      [](Fl_Widget *, void *) { format_partition(); }, 85);
  A.b_resize = button("Resize / Move…", "Change the size or place of the selected partition",
                      [](Fl_Widget *, void *) { resize_partition(); }, 125);
  A.b_edit = button("Name && Flags…", "Change the partition name and flags (esp, boot, ...)",
                    [](Fl_Widget *, void *) { edit_partition(); }, 120);
  A.b_mount = button("Mount", "Mount or unmount the selected partition", [](Fl_Widget *, void *) { mount_toggle(); }, 90);
  Fl_Box *bfill = new Fl_Box(bx, by, 1, 30);
  btns->resizable(bfill);
  btns->end();
  right->resizable(A.table);
  right->end();
  tile->end();
  tile->resizable(right);
  tile->size_range(A.list, 160, 100, 400, 10000);

  Fl_Group *sb = new Fl_Group(0, H - ST, W, ST);
  sb->box(FL_FLAT_BOX);
  sb->color(p.bg);
  A.status = new Fl_Box(8, H - ST, W - 280, ST);
  A.status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  A.status->labelsize(12);
  A.status->labelcolor(p.fg_dim);
  A.progress = new Fl_Progress(W - 264, H - ST + 4, 256, ST - 8);
  A.progress->selection_color(p.accent);
  A.progress->labelsize(11);
  A.progress->hide();
  sb->resizable(A.status);
  sb->end();

  A.win->resizable(tile);
  A.win->end();
  A.win->size_range(720, 460);
  fld::set_window_icon(A.win, "fl-disks");
  Fl::add_handler(keys);

#ifdef FLD_HAVE_UDISKS
  ud::Client::get().on_changed = [] {
    refresh(false);
    A.list->redraw();
    if (!A.busy) show_disk();
  };
  ud::Client::get().start();
#endif
  refresh(true);
  set_status(fld::which("parted").empty() ? "parted is not installed: disks can be shown but not changed."
                                          : "Select a partition or free space.");
  Fl::add_timeout(3.0, tick);
  A.win->show();
  return Fl::run();
}
