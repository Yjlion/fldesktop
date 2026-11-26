// fl-diskusage: what takes the space on a disk. A folder is scanned in the
// background and drawn as a treemap: each rectangle is a file or a folder,
// its area its size. Click a folder to go into it, Backspace or Up to go back.
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <algorithm>
#include <cmath>
#include <memory>
#include <thread>

#include "du_model.h"
#include "fld/file_dialog.h"
#include "fld/menus.h"
#include "fld/mime.h"
#include "fld/text.h"
#include "fld/theme.h"
#include "fld/trash.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fld/window_size.h"

namespace {

std::unique_ptr<du::Node> g_root;  // the last scan
du::Node *g_cur = nullptr;         // the folder shown
du::Progress *g_progress = nullptr; // the scan running now
std::string g_scan_path;
Fl_Box *g_path, *g_status;
Fl_Button *g_up;

Fl_Color tile_color(const du::Node *n, int depth) {
  // folders in the accent's hue by depth, files by their kind
  const fld::Palette &p = fld::palette();
  if (n->dir) return fl_color_average(p.accent, p.bg2, depth == 0 ? 0.55f : 0.35f);
  std::string ext;
  size_t dot = n->name.rfind('.');
  if (dot != std::string::npos && dot > 0) ext = fld::lower(n->name.substr(dot + 1));
  static const struct { const char *exts; Fl_Color c; } kinds[] = {
      {" jpg jpeg png gif webp svg heic raw cr2 nef tiff bmp ", fl_rgb_color(0x4c, 0xaf, 0x50)},
      {" mp4 mkv webm avi mov m4v ", fl_rgb_color(0xe5, 0x73, 0x73)},
      {" mp3 flac ogg opus wav m4a ", fl_rgb_color(0xba, 0x68, 0xc8)},
      {" zip tar gz xz zst bz2 7z rar iso img deb rpm pkg ", fl_rgb_color(0xff, 0xb7, 0x4d)},
      {" pdf doc docx odt txt md epub ", fl_rgb_color(0x4d, 0xb6, 0xac)},
  };
  for (auto &k : kinds)
    if (!ext.empty() && std::string(k.exts).find(" " + ext + " ") != std::string::npos) return k.c;
  return fl_rgb_color(0x90, 0xa4, 0xae);
}

class Treemap : public Fl_Widget {
public:
  std::vector<du::Tile> tiles;
  const du::Node *hover = nullptr;
  int laid_w = 0, laid_h = 0;
  const du::Node *laid_for = nullptr;
  Treemap(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}

  void relayout() {
    tiles.clear();
    laid_for = g_cur;
    laid_w = w();
    laid_h = h();
    if (g_cur) tiles = du::layout(*g_cur, {0, 0, (double)w(), (double)h()}, 3, 18, 20);
  }

  const du::Node *at(int ex, int ey) const {
    const du::Node *found = nullptr;
    for (auto &t : tiles) // deeper tiles come after their folder
      if (ex >= x() + t.r.x && ex < x() + t.r.x + t.r.w && ey >= y() + t.r.y && ey < y() + t.r.y + t.r.h) found = t.node;
    return found;
  }

  void draw() override {
    if (laid_for != g_cur || laid_w != w() || laid_h != h()) relayout();
    const fld::Palette &p = fld::palette();
    fl_push_clip(x(), y(), w(), h());
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    if (!g_cur) {
      fl_color(p.fg_dim);
      fl_font(FL_HELVETICA, 15);
      fl_draw(g_progress ? _("Scanning…") : _("Choose a folder to see what takes its space."), x(), y(), w(), h(),
              FL_ALIGN_CENTER | FL_ALIGN_WRAP);
    }
    for (auto &t : tiles) {
      int tx = x() + (int)lround(t.r.x), ty = y() + (int)lround(t.r.y);
      int tw = (int)lround(t.r.x + t.r.w) - (int)lround(t.r.x), th = (int)lround(t.r.y + t.r.h) - (int)lround(t.r.y);
      if (tw < 1 || th < 1) continue;
      Fl_Color c = tile_color(t.node, t.depth);
      if (t.node == hover) c = fl_color_average(c, FL_WHITE, 0.75f);
      fl_color(c);
      fl_rectf(tx, ty, tw, th);
      fl_color(fl_color_average(c, FL_BLACK, 0.6f));
      fl_rect(tx, ty, tw, th);
      if (tw > 40 && th > 14) {
        fl_font(t.node->dir ? FL_HELVETICA_BOLD : FL_HELVETICA, 11);
        fl_color(fl_contrast(FL_BLACK, c));
        std::string label = t.node->name + "  " + du::human(t.node->size);
        if (fl_width(label.c_str()) > tw - 6) label = t.node->name;
        fl_draw(fld::escape_at(fld::ellipsize(label, tw - 6)).c_str(), tx + 3, ty + 1, tw - 6, 16,
                FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      }
    }
    fl_pop_clip();
  }

  int handle(int e) override;
};

Treemap *g_map;

void show_folder(du::Node *n) {
  g_cur = n;
  g_map->hover = nullptr;
  g_map->redraw();
  if (!n) {
    g_path->label("");
    g_up->deactivate();
    return;
  }
  g_path->copy_label(fld::escape_at(n->path()).c_str());
  n->parent ? g_up->activate() : g_up->deactivate();
  g_status->copy_label(fld::format(_("%s in %s files"), {du::human(n->size), std::to_string(n->files)}).c_str());
}

void progress_tick(void *) {
  if (!g_progress) return;
  g_status->copy_label(fld::format(_("Scanning… %s in %s files"),
                                   {du::human(g_progress->bytes), std::to_string(g_progress->files.load())})
                           .c_str());
  Fl::repeat_timeout(0.25, progress_tick);
}

void start_scan(const std::string &path) {
  if (g_progress) g_progress->cancel = true; // the old scan ends and frees its own state
  g_scan_path = path;
  show_folder(nullptr);
  g_root.reset();
  du::Progress *pr = new du::Progress;
  g_progress = pr;
  g_path->copy_label(fld::escape_at(path).c_str());
  Fl::add_timeout(0.25, progress_tick);
  std::thread([path, pr] {
    du::Node *root = du::scan(path, *pr).release();
    Fl::awake([](void *d) {
      auto *res = (std::pair<du::Node *, du::Progress *> *)d;
      std::unique_ptr<du::Node> root(res->first);
      du::Progress *pr = res->second;
      delete res;
      bool current = pr == g_progress && !pr->cancel;
      delete pr;
      if (!current) return;
      g_progress = nullptr;
      Fl::remove_timeout(progress_tick);
      g_root = std::move(root);
      if (g_root->error) fl_alert("%s", fld::format(_("Cannot read %s."), {g_root->name}).c_str());
      show_folder(g_root.get());
    }, new std::pair<du::Node *, du::Progress *>(root, pr));
  }).detach();
}

void choose_folder() {
  fld::FileDialogOptions o;
  o.directory = true;
  o.title = _("Folder to Analyse");
  o.accept_label = _("Analyse");
  o.folder = g_scan_path.empty() ? fld::home_dir() : g_scan_path;
  fld::FileDialogResult r = fld::run_file_dialog(o);
  if (r.ok && !r.paths.empty()) start_scan(r.paths[0]);
}

void go_up() {
  if (g_cur && g_cur->parent) show_folder(g_cur->parent);
}

void remove_node(const du::Node *n) {
  std::string path = n->path();
  if (fl_choice("%s", _("Cancel"), _("Move to Trash"), nullptr,
                fld::format(_("Move %s (%s) to the trash?"), {path, du::human(n->size)}).c_str()) != 1)
    return;
  std::string err;
  if (!fld::trash(path, &err)) {
    fl_alert("%s", err.c_str());
    return;
  }
  // take it out of the tree and the sizes above it
  du::Node *parent = n->parent;
  uint64_t size = n->size, files = n->files;
  for (du::Node *a = parent; a; a = a->parent) {
    a->size -= std::min(a->size, size);
    a->files -= std::min(a->files, files);
  }
  auto &ch = parent->children;
  ch.erase(std::remove_if(ch.begin(), ch.end(), [n](const std::unique_ptr<du::Node> &c) { return c.get() == n; }),
           ch.end());
  show_folder(g_cur);
}

int Treemap::handle(int e) {
  switch (e) {
    case FL_ENTER: return 1;
    case FL_MOVE: {
      const du::Node *h = at(Fl::event_x(), Fl::event_y());
      if (h != hover) {
        hover = h;
        if (h) g_status->copy_label(fld::escape_at(h->path() + "  —  " + du::human(h->size)).c_str());
        else show_folder(g_cur);
        redraw();
      }
      return 1;
    }
    case FL_LEAVE:
      hover = nullptr;
      if (g_cur) show_folder(g_cur);
      redraw();
      return 1;
    case FL_PUSH: {
      const du::Node *n = at(Fl::event_x(), Fl::event_y());
      if (!n) return 1;
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        fld::Menu m;
        if (n->dir) m.item(_("Show This Folder"), [n] { show_folder(const_cast<du::Node *>(n)); });
        m.item(_("Open"), [n] { fld::open_path(n->path()); });
        m.item(_("Open the Folder in Files"), [n] { fld::spawn({"fl-files", n->dir ? n->path() : n->parent->path()}); });
        m.divider();
        m.item(_("Move to Trash…"), [n] { remove_node(n); });
        m.popup();
        return 1;
      }
      // a click goes into the folder of that tile, below the one shown
      const du::Node *d = n->dir ? n : n->parent;
      while (d && d->parent && d->parent != g_cur) d = d->parent;
      if (d && d != g_cur && d->dir) show_folder(const_cast<du::Node *>(d));
      return 1;
    }
    case FL_KEYBOARD:
      if (Fl::event_key() == FL_BackSpace || (Fl::event_key() == FL_Up && Fl::event_state(FL_ALT))) {
        go_up();
        return 1;
      }
      return 0;
    case FL_FOCUS:
    case FL_UNFOCUS: return 1;
  }
  return Fl_Widget::handle(e);
}

} // namespace

int main(int argc, char **argv) {
  fld::init_app("fl-diskusage", argc, argv);
  Fl::lock();
  const int W = 960, H = 640, TB = 44, SB = 28;
  const fld::Palette &p = fld::palette();
  Fl_Double_Window *win = new Fl_Double_Window(W, H, _("Disk Usage"));
  Fl_Group *top = new Fl_Group(0, 0, W, TB);
  top->box(FL_FLAT_BOX);
  top->color(p.bg);
  Fl_Button *open = new Fl_Button(8, 7, 110, 30, _("Folder…"));
  open->tooltip(_("Choose the folder to analyse (Ctrl+O)"));
  open->shortcut(FL_CTRL + 'o');
  open->callback([](Fl_Widget *, void *) { choose_folder(); });
  Fl_Button *home = new Fl_Button(124, 7, 80, 30, _("Home"));
  home->callback([](Fl_Widget *, void *) { start_scan(fld::home_dir()); });
  Fl_Button *rootb = new Fl_Button(210, 7, 80, 30, _("Whole Disk"));
  rootb->tooltip(_("The system disk (/); folders you may not read are left out"));
  rootb->callback([](Fl_Widget *, void *) { start_scan("/"); });
  g_up = new Fl_Button(296, 7, 60, 30, _("Up"));
  g_up->tooltip(_("The folder above (Backspace)"));
  g_up->callback([](Fl_Widget *, void *) { go_up(); });
  g_up->deactivate();
  Fl_Button *rescan = new Fl_Button(362, 7, 90, 30, _("Rescan"));
  rescan->shortcut(FL_F + 5);
  rescan->callback([](Fl_Widget *, void *) {
    if (!g_scan_path.empty()) start_scan(g_scan_path);
  });
  g_path = new Fl_Box(460, 7, W - 468, 30);
  g_path->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_path->labelfont(FL_HELVETICA_BOLD);
  top->resizable(g_path);
  top->end();
  g_map = new Treemap(0, TB, W, H - TB - SB);
  g_status = new Fl_Box(0, H - SB, W, SB);
  g_status->box(FL_FLAT_BOX);
  g_status->color(p.bg);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  win->end();
  win->resizable(g_map);
  win->size_range(480, 320);
  fld::remember_window_size(win, "fl-diskusage");
  win->show(argc, argv);
  g_map->take_focus();
  std::string start;
  for (int i = 1; i < argc; i++)
    if (argv[i][0] != '-') start = argv[i];
  start_scan(start.empty() ? fld::home_dir() : fld::expand_tilde(start));
  return Fl::run();
}
