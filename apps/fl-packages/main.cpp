// fl-packages: software updates and packages for fldesktop (PackageKit)
//
//   fl-packages                     the window (updates first when there are any)
//   fl-packages --updates|--search TERM
//   fl-packages FILE...             install local package files
//   fl-packages --check-updates     background checker started at login
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Browser.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_ask.H>
#include <glib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <cerrno>
#include <sys/wait.h>
#include <malloc.h>
#include <string>
#include <vector>

#include "fld/menus.h"
#include "fld/icons.h"
#ifdef HAVE_APPSTREAM
#include "app_icons.h"
#endif
#include "fld/ini.h"
#include "fld/notify.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "pkclient.h"

namespace {

// ---- transaction progress -----------------------------------------------------------

class ProgressWindow : public Fl_Double_Window {
public:
  Fl_Box *status, *package;
  Fl_Progress *bar;
  Fl_Button *cancel;
  pk::Job job;
  ProgressWindow(const std::string &title) : Fl_Double_Window(440, 150) {
    copy_label(title.c_str());
    status = new Fl_Box(16, 14, 408, 24, "Starting");
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    status->labelfont(FL_HELVETICA_BOLD);
    package = new Fl_Box(16, 38, 408, 22);
    package->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    package->labelcolor(fld::palette().fg_dim);
    bar = new Fl_Progress(16, 66, 408, 22);
    bar->minimum(0);
    bar->maximum(100);
    bar->selection_color(fld::palette().accent);
    cancel = new Fl_Button(324, 104, 100, 30, "Cancel");
    cancel->callback([](Fl_Widget *w, void *) {
      ProgressWindow *p = (ProgressWindow *)w->window();
      p->job.cancel();
      w->deactivate();
    });
    end();
    set_modal();
    callback([](Fl_Widget *, void *) {}); // closing is not allowed; use Cancel
    fld::set_window_icon(this, "fl-packages");
  }
  void update(const pk::Progress &p) {
    status->copy_label(p.status.c_str());
    package->copy_label(fld::escape_at(p.package).c_str());
    if (p.percent >= 0) bar->value((float)p.percent);
    p.allow_cancel ? cancel->activate() : cancel->deactivate();
    redraw();
  }
};

// ---- the apply flow: simulate, confirm, run, retry untrusted ------------------------

enum class Action { INSTALL, REMOVE, UPDATE, FILES };

pk::Job start(Action a, const std::vector<std::string> &ids, bool simulate, bool trusted,
              std::function<void(const std::vector<pk::Package> &, const pk::Error &)> done, pk::ProgressFn p) {
  switch (a) {
  case Action::INSTALL: return pk::install(ids, simulate, trusted, done, p);
  case Action::REMOVE: return pk::remove(ids, simulate, true, done, p);
  case Action::UPDATE: return pk::update(ids, simulate, trusted, done, p);
  case Action::FILES: return pk::install_files(ids, simulate, trusted, done, p);
  }
  return {};
}

bool confirm_changes(Action a, const std::vector<std::string> &ids, const std::vector<pk::Package> &changes) {
  std::set<std::string> asked;
  for (auto &i : ids) asked.insert(pk::parse_id(i).name);
  std::map<std::string, std::vector<std::string>> by;
  bool extra = false;
  for (auto &p : changes) {
    std::string kind;
    switch (p.info) {
    case 12 /*INSTALLING*/: kind = "will be installed"; break;
    case 13 /*REMOVING*/: kind = "will be removed"; break;
    case 11 /*UPDATING*/: kind = "will be updated"; break;
    case 18 /*DOWNGRADING*/: kind = "will be downgraded"; break;
    case 17 /*REINSTALLING*/: kind = "will be reinstalled"; break;
    case 19 /*OBSOLETING*/: kind = "will be replaced"; break;
    default: kind = "will be changed"; break;
    }
    by[kind].push_back(p.name + " " + p.version);
    if (!asked.count(p.name)) extra = true;
  }
  // nothing beyond what was asked for an install or update: no question
  if (!extra && a != Action::REMOVE) return true;
  Fl_Double_Window w(520, 420, "Confirm Changes");
  Fl_Box *t = new Fl_Box(16, 12, 488, 40,
                         a == Action::REMOVE ? "Removing these packages also changes others:"
                                             : "These additional changes are needed:");
  t->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  t->labelfont(FL_HELVETICA_BOLD);
  Fl_Text_Display *d = new Fl_Text_Display(16, 56, 488, 304);
  Fl_Text_Buffer *buf = new Fl_Text_Buffer;
  d->buffer(buf);
  std::string text;
  for (auto &kv : by) {
    text += std::to_string(kv.second.size()) + " " + (kv.second.size() == 1 ? "package " : "packages ") + kv.first +
            ":\n";
    std::sort(kv.second.begin(), kv.second.end());
    for (auto &n : kv.second) text += "    " + n + "\n";
    text += "\n";
  }
  buf->text(text.c_str());
  Fl_Button *no = new Fl_Button(296, 374, 100, 30, "Cancel");
  Fl_Return_Button *yes = new Fl_Return_Button(404, 374, 100, 30, "Continue");
  static int result;
  result = 0;
  no->callback([](Fl_Widget *b, void *) { result = 0; b->window()->hide(); });
  yes->callback([](Fl_Widget *b, void *) { result = 1; b->window()->hide(); });
  w.end();
  w.set_modal();
  w.show();
  while (w.shown()) Fl::wait();
  d->buffer(nullptr);
  delete buf;
  return result == 1;
}

// Runs the whole flow; `done(ok)` afterwards (FLTK thread).
void apply(Action a, const std::vector<std::string> &ids, const std::string &title, std::function<void(bool)> done) {
  auto win = std::make_shared<ProgressWindow *>(new ProgressWindow(title));
  (*win)->status->label("Checking dependencies");
  (*win)->show();
  auto progress = [win](const pk::Progress &p) {
    if (*win) (*win)->update(p);
  };
  auto finish = [win, done](bool ok) {
    if (*win) {
      (*win)->hide();
      Fl::delete_widget(*win);
      *win = nullptr;
    }
    if (done) done(ok);
  };
  std::function<void(bool)> run_real;
  auto run_real_ptr = std::make_shared<std::function<void(bool)>>();
  *run_real_ptr = [a, ids, title, win, progress, finish, run_real_ptr](bool trusted) {
    (*win)->job = start(a, ids, false, trusted, [a, ids, title, finish, run_real_ptr, trusted](
                                                    const std::vector<pk::Package> &, const pk::Error &e) {
      if (e && e.untrusted() && trusted) {
        if (fl_choice("The packages are not signed by a trusted key.\n\n%s\n\nInstall them anyway?", "Cancel",
                      "Install Anyway", nullptr, e.text.c_str()) == 1) {
          (*run_real_ptr)(false);
          return;
        }
        finish(false);
        return;
      }
      if (e) {
        if (e.code != -1) fl_alert("%s failed.\n\n%s", title.c_str(), e.text.c_str());
        finish(false);
        return;
      }
      finish(true);
    }, progress);
  };
  // FILES: simulation needs the files; PackageKit handles it the same way
  (*win)->job = start(a, ids, true, true, [a, ids, win, finish, run_real_ptr, title](
                                              const std::vector<pk::Package> &changes, const pk::Error &e) {
    if (e && !e.untrusted()) {
      if (e.code != -1) fl_alert("%s is not possible.\n\n%s", title.c_str(), e.text.c_str());
      finish(false);
      return;
    }
    if (!e && !confirm_changes(a, ids, changes)) {
      finish(false);
      return;
    }
    (*win)->status->label("Starting");
    (*run_real_ptr)(true);
  }, progress);
}

// ---- main window -------------------------------------------------------------------------


// ---- application icons (AppStream) ---------------------------------------------------

#ifdef HAVE_APPSTREAM
static std::map<std::string, appicons::AppInfo> g_apps; // pkgname -> app
static bool g_apps_ready = false;
class MainWindow;
static MainWindow *g_main_win;
// Reads the AppStream catalog for application names and icons, once, when
// something needs them (the Software tab, a list of updates): it takes a
// moment and about 90 MB while it is read.
static void ensure_catalog();
#endif

static const char *app_name(const std::string &pkg) {
#ifdef HAVE_APPSTREAM
  auto it = g_apps.find(pkg);
  if (it != g_apps.end() && !it->second.name.empty()) return it->second.name.c_str();
#endif
  (void)pkg;
  return nullptr;
}

// The package's application icon at `size`, or the generic package icon.
static Fl_Image *package_icon(const std::string &pkg, int size) {
  static std::map<std::string, Fl_Image *> cache;
  std::string key = pkg + "@" + std::to_string(size);
  auto c = cache.find(key);
  if (c != cache.end()) return c->second;
  Fl_Image *img = nullptr;
#ifdef HAVE_APPSTREAM
  auto it = g_apps.find(pkg);
  if (it != g_apps.end()) {
    const appicons::AppInfo &a = it->second;
    if (!a.stock.empty()) img = fld::icon(a.stock, size);
    if (!img && !a.icon_file.empty()) {
      std::unique_ptr<Fl_Image> full;
      if (fld::ends_with(a.icon_file, ".jxl")) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (appicons::decode_jxl(a.icon_file, w, h, rgba)) {
          Fl_RGB_Image tmp(rgba.data(), w, h, 4);
          full.reset(tmp.copy()); // owns its pixels
        }
      } else {
        full.reset(fld::load_image(a.icon_file));
      }
      if (full && full->w() > 0 && full->h() > 0) img = full->copy(size, size);
    }
  }
#endif
  if (!img) {
    // the generic icon; not remembered while the catalog is still loading
    img = fld::icon_first({"package-x-generic", "application-x-executable"}, size);
#ifdef HAVE_APPSTREAM
    if (!g_apps_ready) return img;
#endif
  }
  cache[key] = img;
  return img;
}

// Fl_Check_Browser with the package icon in front of each line ("name ...")
class IconCheckBrowser : public Fl_Check_Browser {
public:
  using Fl_Check_Browser::Fl_Check_Browser;
protected:
  int item_height(void *item) const override { return std::max(Fl_Check_Browser::item_height(item), 22); }
  void item_draw(void *item, int X, int Y, int W, int H) const override {
    std::string t = item_text(item);
    std::string name = t.substr(0, t.find(' '));
    Fl_Check_Browser::item_draw(item, X, Y, W, H);
    // the check box is drawn first, at the left; the icon goes right after
    // it, over the start of the text, which is indented with spaces
    int cb = textsize() + 4;
    if (Fl_Image *ic = package_icon(name, 18)) ic->draw(X + cb + 4, Y + (H - ic->h()) / 2);
  }
};

class MainWindow : public Fl_Double_Window {
public:
  Fl_Tabs *tabs;
  // updates
  Fl_Group *upd_tab;
  Fl_Box *upd_summary;
  Fl_Check_Browser *upd_list;
  Fl_Text_Display *upd_detail;
  Fl_Box *upd_none; // in place of the list and details when there is nothing to update
  Fl_Text_Buffer *upd_buf;
  Fl_Button *upd_check, *upd_install;
  std::vector<pk::Package> updates;
  // software
  Fl_Group *sw_tab;
  Fl_Input *sw_search;
  Fl_Choice *sw_where, *sw_show;
  Fl_Hold_Browser *sw_groups, *sw_list;
  Fl_Text_Display *sw_detail;
  Fl_Text_Buffer *sw_buf;
  Fl_Button *sw_action;
  Fl_Box *sw_icon = nullptr, *sw_name = nullptr; // the selected package's application
  std::vector<pk::Package> results;
  // details tabs of the selected package
  Fl_Tabs *info_tabs;
  Fl_Group *tab_details, *tab_files, *tab_deps, *tab_rdeps;
  Fl_Input *files_filter;
  Fl_Browser *files_list;
  Fl_Hold_Browser *deps_list, *rdeps_list;
  Fl_Check_Button *deps_recursive, *rdeps_recursive;
  pk::Package info_pkg;          // the package the tabs show
  std::vector<std::string> files;
  std::vector<pk::Package> deps, rdeps;
  bool files_loaded = false, deps_loaded = false, rdeps_loaded = false;
  pk::Job info_job;
  std::string pending_select;    // select this name when results arrive
  // repositories
  Fl_Group *repo_tab;
  Fl_Check_Browser *repo_list;
  std::vector<pk::Repo> repos;
  // history
  Fl_Group *hist_tab;
  Fl_Browser *hist_list;
  Fl_Box *status;
  pk::Job busy;

  MainWindow() : Fl_Double_Window(980, 620, "Software Packages") {
    const int W = w(), H = h();
    tabs = new fld::Tabs(8, 8, W - 16, H - 40);
    // --- updates
    upd_tab = new Fl_Group(8, 38, W - 16, H - 70, "Updates");
    upd_summary = new Fl_Box(20, 48, W - 300, 28);
    upd_summary->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    upd_summary->labelfont(FL_HELVETICA_BOLD);
    upd_check = new Fl_Button(W - 272, 48, 120, 28, "Check Now");
    upd_check->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->check_updates(true); }, this);
    upd_install = new Fl_Button(W - 146, 48, 126, 28, "Install Updates");
    upd_install->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->install_updates(); }, this);
    upd_list = new IconCheckBrowser(20, 84, W - 40, (H - 120) / 2);
    upd_list->textsize(13);
    upd_list->when(FL_WHEN_CHANGED);
    upd_list->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->update_selected(); }, this);
    upd_detail = new Fl_Text_Display(20, 92 + (H - 120) / 2, W - 40, H - 120 - (H - 120) / 2 - 14);
    upd_buf = new Fl_Text_Buffer;
    upd_detail->buffer(upd_buf);
    upd_detail->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    upd_none = new Fl_Box(20, 84, W - 40, H - 126, "There are no updates to install.");
    // a check mark above the sentence, so the page does not look empty
    upd_none->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER | FL_ALIGN_WRAP | FL_ALIGN_IMAGE_OVER_TEXT);
    upd_none->labelcolor(fld::palette().fg_dim);
    upd_none->labelsize(16);
    if (Fl_Image *ok = fld::symbolic_icon("object-select", 64, fld::palette().accent)) upd_none->image(ok);
    upd_none->hide();
    upd_tab->resizable(upd_detail);
    upd_tab->end();
    // --- software
    sw_tab = new Fl_Group(8, 38, W - 16, H - 70, "Software");
    sw_search = new Fl_Input(20, 48, 320, 28);
    sw_search->when(FL_WHEN_ENTER_KEY_ALWAYS);
    sw_search->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->do_search(); }, this);
    Fl_Button *go = new Fl_Button(346, 48, 90, 28, "Search");
    go->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->do_search(); }, this);
    sw_where = new Fl_Choice(446, 48, 150, 28);
    sw_where->add("Names");
    sw_where->add("Descriptions");
    sw_where->value(0);
    sw_show = new Fl_Choice(604, 48, 160, 28);
    sw_show->add("All packages");
    sw_show->add("Installed");
    sw_show->add("Not installed");
    sw_show->value(0);
    sw_groups = new Fl_Hold_Browser(20, 84, 180, H - 120);
    sw_groups->textsize(13);
    sw_groups->add("Installed packages");
    for (auto &g : pk::groups()) sw_groups->add(pk::group_text(g).c_str());
    sw_groups->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->browse_group(); }, this);
    sw_list = new Fl_Hold_Browser(208, 84, W - 228, (H - 120) / 2 + 40);
    sw_list->format_char(0);
    sw_list->column_char('\t');
    sw_list->textsize(13);
    static int widths[] = {200, 150, 110, 0};
    sw_list->column_widths(widths);
    sw_list->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->result_selected(); }, this);
    int dy = 84 + (H - 120) / 2 + 48;
    {
      int tx = 208, tw = W - 228 - 140, th = H - 36 - dy;
      info_tabs = new fld::Tabs(tx, dy, tw, th);
      int cy = dy + 26, ch = th - 26;
      tab_details = new Fl_Group(tx, cy, tw, ch, "Details");
      sw_detail = new Fl_Text_Display(tx, cy, tw, ch);
      sw_buf = new Fl_Text_Buffer;
      sw_detail->buffer(sw_buf);
      sw_detail->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
      tab_details->resizable(sw_detail);
      tab_details->end();
      tab_files = new Fl_Group(tx, cy, tw, ch, "Files");
      files_filter = new Fl_Input(tx + 4, cy + 4, tw - 8, 26);
      files_filter->tooltip("Show only the files whose path contains this text");
      files_filter->when(FL_WHEN_CHANGED);
      files_filter->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->show_files(); }, this);
      files_list = new Fl_Browser(tx, cy + 34, tw, ch - 34);
      files_list->type(FL_HOLD_BROWSER);
      files_list->format_char(0);
      files_list->textsize(12);
      files_list->textfont(FL_COURIER);
      files_list->tooltip("Double-click to open the folder");
      files_list->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->file_clicked(); }, this);
      tab_files->resizable(files_list);
      tab_files->end();
      auto dep_tab = [&](const char *label, Fl_Hold_Browser *&list, Fl_Check_Button *&rec) {
        Fl_Group *g = new Fl_Group(tx, cy, tw, ch, label);
        rec = new fld::CheckButton(tx + 4, cy + 4, tw - 8, 26, "Include indirect ones");
        rec->callback([](Fl_Widget *w, void *d) { ((MainWindow *)d)->reload_deps(w); }, this);
        list = new Fl_Hold_Browser(tx, cy + 34, tw, ch - 34);
        list->format_char(0);
        list->column_char('\t');
        list->textsize(13);
        static int dw[] = {200, 150, 0};
        list->column_widths(dw);
        list->tooltip("Double-click to show the package");
        list->callback([](Fl_Widget *w, void *d) { ((MainWindow *)d)->dep_clicked((Fl_Hold_Browser *)w); }, this);
        g->resizable(list);
        g->end();
        return g;
      };
      tab_deps = dep_tab("Depends on", deps_list, deps_recursive);
      tab_rdeps = dep_tab("Required by", rdeps_list, rdeps_recursive);
      info_tabs->end();
      info_tabs->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->info_tab_changed(); }, this);
      info_tabs->resizable(tab_details);
    }
    sw_icon = new Fl_Box(W - 152, dy + 40, 132, 72);
    sw_name = new Fl_Box(W - 152, dy + 114, 132, 40);
    sw_name->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP | FL_ALIGN_WRAP);
    sw_name->labelfont(FL_HELVETICA_BOLD);
    sw_name->labelsize(13);
    sw_action = new Fl_Button(W - 152, dy, 132, 30, "Install");
    sw_action->deactivate();
    sw_action->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->install_or_remove(); }, this);
    sw_tab->resizable(info_tabs);
    sw_tab->end();
    // --- repositories
    repo_tab = new Fl_Group(8, 38, W - 16, H - 70, "Repositories");
    Fl_Box *rh = new Fl_Box(20, 48, W - 40, 28, "Software sources (check to enable):");
    rh->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    repo_list = new Fl_Check_Browser(20, 80, W - 40, H - 116);
    repo_list->textsize(13);
    repo_list->when(FL_WHEN_CHANGED);
    repo_list->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->repo_toggled(); }, this);
    repo_tab->resizable(repo_list);
    repo_tab->end();
    // --- history
    hist_tab = new Fl_Group(8, 38, W - 16, H - 70, "History");
    hist_list = new Fl_Browser(20, 48, W - 40, H - 84);
    hist_list->type(FL_HOLD_BROWSER);
    hist_list->format_char(0);
    hist_list->textsize(13);
    hist_tab->resizable(hist_list);
    hist_tab->end();
    tabs->end();
    tabs->callback([](Fl_Widget *, void *d) { ((MainWindow *)d)->tab_changed(); }, this);
    status = new Fl_Box(12, H - 30, W - 24, 26);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    status->labelcolor(fld::palette().fg_dim);
    end();
    resizable(tabs);
    size_range(760, 480);
    fld::set_window_icon(this, "fl-packages");
    make_context_menus();
  }

  // right-click menus for the lists
  void make_context_menus() {
    auto copy = [](const std::string &t) { Fl::copy(t.c_str(), (int)t.size(), 1); };
    // updates: a check list (a click would toggle the check)
    fld::set_context_menu(upd_list, [this](fld::Menu &m) {
      m.button(upd_install).button(upd_check).divider();
      m.item("Select All", [this] {
        upd_list->check_all();
        update_selected();
      }, upd_list->nitems() ? 0 : FL_MENU_INACTIVE);
      m.item("Select None", [this] {
        upd_list->check_none();
        update_selected();
      }, upd_list->nitems() ? 0 : FL_MENU_INACTIVE);
    }, false);
    fld::set_context_menu(sw_list, [this, copy](fld::Menu &m) {
      bool sel = sw_list->value() > 0 && sw_action->active();
      m.button(sw_action).divider();
      auto show = [this](Fl_Group *tab) {
        return [this, tab] {
          info_tabs->value(tab);
          info_tab_changed();
        };
      };
      m.item("Details", show(tab_details), sel ? 0 : FL_MENU_INACTIVE);
      m.item("Files", show(tab_files), sel ? 0 : FL_MENU_INACTIVE);
      m.item("Depends On", show(tab_deps), sel ? 0 : FL_MENU_INACTIVE);
      m.item("Required By", show(tab_rdeps), sel ? 0 : FL_MENU_INACTIVE).divider();
      std::string name = sw_name->label() ? sw_name->label() : "";
      m.item("Copy Name", [copy, name] { copy(name); }, sel && !name.empty() ? 0 : FL_MENU_INACTIVE);
    });
    fld::set_context_menu(files_list, [this, copy](fld::Menu &m) {
      int i = files_list->value();
      std::string f = i > 0 ? files_list->text(i) : "";
      bool ok = !f.empty() && f[0] == '/';
      m.item("Open Folder", [f] { fld::spawn({"xdg-open", fld::is_dir(f) ? f : fld::dirname(f)}); },
             ok ? 0 : FL_MENU_INACTIVE);
      m.item("Copy Path", [copy, f] { copy(f); }, ok ? 0 : FL_MENU_INACTIVE);
    });
    for (Fl_Hold_Browser *list : {deps_list, rdeps_list})
      fld::set_context_menu(list, [this, list, copy](fld::Menu &m) {
        auto &v = list == deps_list ? deps : rdeps;
        int i = list->value() - 1;
        bool ok = i >= 0 && i < (int)v.size();
        std::string name = ok ? v[(size_t)i].name : "";
        m.item("Show Package", [this, list] {
          Fl::e_clicks = 1; // as a double click
          dep_clicked(list);
          Fl::e_clicks = 0;
        }, ok ? 0 : FL_MENU_INACTIVE);
        m.item("Copy Name", [copy, name] { copy(name); }, ok ? 0 : FL_MENU_INACTIVE);
      });
    fld::set_context_menu(hist_list, [this, copy](fld::Menu &m) {
      int i = hist_list->value();
      std::string t = i > 0 ? fld::trim(hist_list->text(i)) : "";
      m.item("Copy Line", [copy, t] { copy(t); }, t.empty() ? FL_MENU_INACTIVE : 0);
    });
  }

  void set_status(const std::string &s) {
    status->copy_label(fld::escape_at(s).c_str());
    status->redraw();
  }

  // --- updates
  void check_updates(bool refresh) {
    upd_check->deactivate();
    upd_install->deactivate();
    upd_summary->label("Checking for updates…");
    show_update_list(true);
    auto get = [this] {
      pk::get_updates([this](const std::vector<pk::Package> &list, const pk::Error &e) {
        upd_check->activate();
        if (e) {
          upd_summary->label("Could not check for updates");
          set_status(e.text);
          return;
        }
        set_updates(list);
      }, [this](const pk::Progress &p) { set_status(p.status); });
    };
    if (refresh)
      pk::refresh_cache(true, [this, get](const pk::Error &e) {
        if (e && e.code != -1) set_status("Refresh failed: " + e.text);
        get();
      }, [this](const pk::Progress &p) { set_status(p.status + (p.percent >= 0 ? " " + std::to_string(p.percent) + "%" : "")); });
    else get();
  }

  void show_update_list(bool on) {
    for (Fl_Widget *w : {(Fl_Widget *)upd_list, (Fl_Widget *)upd_detail}) on ? w->show() : w->hide();
    on ? upd_none->hide() : upd_none->show();
    if (!on) {
      // when the package lists were last fetched
      pk::time_since_refresh([this](unsigned s) {
        std::string when = s == 0 || s == ~0u ? ""
                           : s < 3600         ? std::to_string(std::max(1u, s / 60)) + " min ago"
                           : s < 2 * 86400    ? std::to_string(s / 3600) + " h ago"
                                              : std::to_string(s / 86400) + " days ago";
        upd_none->copy_label(("There are no updates to install." + (when.empty() ? "" : "\nLast checked " + when + "."))
                                 .c_str());
        upd_none->redraw();
      });
    }
  }

  void set_updates(const std::vector<pk::Package> &list) {
    updates = list;
    if (!list.empty()) ensure_catalog(); // their names and icons
    std::sort(updates.begin(), updates.end(), [](const pk::Package &a, const pk::Package &b) {
      bool as = a.info == 9 /*SECURITY*/, bs = b.info == 9;
      if (as != bs) return as;
      return a.name < b.name;
    });
    upd_list->clear();
    int security = 0;
    for (auto &p : updates) {
      if (p.info == 9) security++;
      std::string kind = pk::info_text(p.info);
      const char *an = app_name(p.name);
      std::string line = p.name + "  " + p.version + (an ? std::string("  ") + an : "") +
                         (p.repo.empty() ? "" : "  (" + p.repo + ")") +
                         (kind == "Update" || kind.empty() ? "" : "  — " + kind);
      // room for the icon IconCheckBrowser draws after the check box
      upd_list->add(("      " + line).c_str(), 1);
    }
    std::string s = updates.empty() ? "The system is up to date"
                                    : std::to_string(updates.size()) + (updates.size() == 1 ? " update" : " updates") +
                                          (security ? ", " + std::to_string(security) + " for security" : "");
    upd_summary->copy_label(s.c_str());
    updates.empty() ? upd_install->deactivate() : upd_install->activate();
    show_update_list(!updates.empty());
    upd_buf->text("");
    set_status("");
    redraw();
  }

  void update_selected() {
    int i = upd_list->value() - 1;
    if (i < 0 || i >= (int)updates.size()) return;
    upd_buf->text("Loading…");
    pk::get_update_detail({updates[i].id}, [this, i](const std::vector<pk::UpdateDetail> &d, const pk::Error &e) {
      if (i >= (int)updates.size()) return;
      std::string t = updates[i].name + " " + updates[i].version + "\n" + updates[i].summary + "\n\n";
      if (e) t += e.text;
      for (auto &x : d) {
        if (!x.update_text.empty()) t += x.update_text + "\n\n";
        if (!x.changelog.empty()) t += "Changes:\n" + x.changelog + "\n\n";
        for (auto &c : x.cves) t += "Security issue: " + c + "\n";
        for (auto &b : x.bugzilla) t += "Bug: " + b + "\n";
        if (x.restart >= 3) t += "\nA restart is needed after this update.\n";
        if (!x.issued.empty()) t += "\nIssued: " + x.issued + "\n";
      }
      upd_buf->text(t.c_str());
    });
  }

  void install_updates() {
    std::vector<std::string> ids;
    for (int i = 1; i <= upd_list->nitems(); i++)
      if (upd_list->checked(i) && i - 1 < (int)updates.size()) ids.push_back(updates[i - 1].id);
    if (ids.empty()) return;
    apply(Action::UPDATE, ids, "Installing updates", [this, n = ids.size()](bool ok) {
      if (ok) {
        fld::Notification note;
        note.summary = "Updates installed";
        note.body = std::to_string(n) + (n == 1 ? " package was updated." : " packages were updated.");
        note.icon = "fl-packages";
        note.transient = true;
        fld::notify(note);
      }
      check_updates(false);
      load_history();
    });
  }

  // --- software
  int show_filter() const { return sw_show->value() == 1 ? 1 : sw_show->value() == 2 ? 0 : -1; }

  void show_results(const std::vector<pk::Package> &list, const pk::Error &e) {
    results = list;
    std::sort(results.begin(), results.end(), [](const pk::Package &a, const pk::Package &b) { return a.name < b.name; });
    sw_list->clear();
    for (auto &p : results) {
      const char *an = app_name(p.name);
      std::string line = p.name + "\t" + p.version + "\t" + (p.installed() ? "installed" : p.repo) + "\t" +
                         (an ? std::string(an) + " – " : "") + p.summary;
      sw_list->add(line.c_str());
      sw_list->icon(sw_list->size(), package_icon(p.name, 20));
    }
    set_status(e ? e.text : std::to_string(results.size()) + " packages");
    sw_buf->text("");
    sw_action->deactivate();
    clear_info();
    if (!pending_select.empty()) {
      for (size_t i = 0; i < results.size(); i++)
        if (results[i].name == pending_select) {
          sw_list->value((int)i + 1);
          sw_list->middleline((int)i + 1);
          result_selected();
          break;
        }
      pending_select.clear();
    }
  }

  // ---- files and dependencies of the selected package ----

  // the AppStream catalog arrived: icons for what is listed
  void refresh_icons() {
    for (size_t i = 0; i < results.size() && (int)i < sw_list->size(); i++) {
      const pk::Package &p = results[i];
      const char *an = app_name(p.name);
      if (an) {
        std::string line = p.name + "\t" + p.version + "\t" + (p.installed() ? "installed" : p.repo) + "\t" + an +
                           " – " + p.summary;
        sw_list->text((int)i + 1, line.c_str());
      }
      sw_list->icon((int)i + 1, package_icon(p.name, 20));
    }
    if (!info_pkg.name.empty()) {
      sw_icon->image(package_icon(info_pkg.name, 64));
      const char *an = app_name(info_pkg.name);
      sw_name->copy_label(fld::escape_at(an ? an : info_pkg.name).c_str());
    }
    redraw();
  }

  void clear_info() {
    info_job.cancel();
    if (sw_icon) {
      sw_icon->image(nullptr);
      sw_name->label(nullptr);
    }
    info_pkg = pk::Package();
    files.clear();
    deps.clear();
    rdeps.clear();
    files_loaded = deps_loaded = rdeps_loaded = false;
    files_list->clear();
    deps_list->clear();
    rdeps_list->clear();
  }

  void info_tab_changed() {
    if (info_pkg.id.empty()) return;
    Fl_Widget *t = info_tabs->value();
    if (t == tab_files && !files_loaded) load_files();
    else if (t == tab_deps && !deps_loaded) load_deps(false);
    else if (t == tab_rdeps && !rdeps_loaded) load_deps(true);
  }

  void load_files() {
    files_loaded = true;
    files_list->clear();
    files_list->add("Loading…");
    std::string id = info_pkg.id;
    info_job = pk::get_files({id}, [this, id](const std::vector<std::string> &f, const pk::Error &e) {
      if (id != info_pkg.id) return;
      files = f;
      show_files();
      if (e) {
        files_list->clear();
        files_list->add(e.text.c_str());
      } else if (files.empty()) {
        files_list->add(info_pkg.installed() ? "(no files)" : "The file list of a package that is not installed is not available here.");
      }
    });
  }

  void show_files() {
    files_list->clear();
    for (auto &f : pk::filter_files(files, files_filter->value())) files_list->add(f.c_str());
  }

  void file_clicked() {
    if (Fl::event_clicks() == 0) return;  // a double click
    int i = files_list->value();
    if (i <= 0) return;
    std::string f = files_list->text(i);
    if (f.empty() || f[0] != '/') return;
    std::string dir = fld::is_dir(f) ? f : fld::dirname(f);
    fld::spawn({"xdg-open", dir});
  }

  void load_deps(bool reverse) {
    Fl_Hold_Browser *list = reverse ? rdeps_list : deps_list;
    (reverse ? rdeps_loaded : deps_loaded) = true;
    list->clear();
    list->add("Loading…");
    std::string id = info_pkg.id;
    bool rec = (reverse ? rdeps_recursive : deps_recursive)->value();
    auto done = [this, id, reverse, list](const std::vector<pk::Package> &l, const pk::Error &e) {
      if (id != info_pkg.id) return;
      auto u = pk::unique_by_name(l);
      // a package does not depend on itself
      u.erase(std::remove_if(u.begin(), u.end(), [&](const pk::Package &p) { return p.name == info_pkg.name; }),
              u.end());
      (reverse ? rdeps : deps) = u;
      list->clear();
      for (auto &p : u)
        list->add((p.name + "\t" + p.version + "\t" + (p.installed() ? "installed" : p.repo)).c_str());
      if (e) list->add(e.text.c_str());
      else if (u.empty()) list->add(reverse ? "No installed package needs it." : "It needs no other package.");
    };
    info_job = reverse ? pk::required_by({id}, rec, done) : pk::depends_on({id}, rec, done);
  }

  void reload_deps(Fl_Widget *w) {
    if (info_pkg.id.empty()) return;
    load_deps(w == rdeps_recursive);
  }

  void dep_clicked(Fl_Hold_Browser *list) {
    if (Fl::event_clicks() == 0) return;
    auto &v = list == deps_list ? deps : rdeps;
    int i = list->value() - 1;
    if (i < 0 || i >= (int)v.size()) return;
    // show that package: search for its name and select it
    pending_select = v[i].name;
    sw_search->value(v[i].name.c_str());
    sw_where->value(0);
    sw_show->value(0);
    do_search();
  }

  void do_search() {
    std::string q = fld::trim(sw_search->value());
    if (q.empty()) return;
    sw_groups->value(0);
    busy.cancel();
    set_status("Searching…");
    busy = pk::search(q, sw_where->value() == 1, show_filter(),
                      [this](const std::vector<pk::Package> &l, const pk::Error &e) { show_results(l, e); });
  }

  void browse_group() {
    int i = sw_groups->value() - 1;
    if (i < 0) return;
    busy.cancel();
    set_status("Loading…");
    if (i == 0) {
      busy = pk::installed_packages([this](const std::vector<pk::Package> &l, const pk::Error &e) { show_results(l, e); });
      return;
    }
    busy = pk::search_group(pk::groups()[i - 1], show_filter(),
                            [this](const std::vector<pk::Package> &l, const pk::Error &e) { show_results(l, e); });
  }

  void result_selected() {
    int i = sw_list->value() - 1;
    if (i < 0 || i >= (int)results.size()) return;
    const pk::Package p = results[i];
    clear_info();
    info_pkg = p;
    sw_icon->image(package_icon(p.name, 64));
    const char *an = app_name(p.name);
    sw_name->copy_label(fld::escape_at(an ? an : p.name).c_str());
    sw_icon->parent()->redraw();
    info_tab_changed();  // a files / dependencies tab that is open loads now
    sw_action->label(p.installed() ? "Remove" : "Install");
    sw_action->activate();
    sw_buf->text((p.name + " " + p.version + "\n" + p.summary + "\n\nLoading…").c_str());
    pk::get_details({p.id}, [this, p](const std::vector<pk::Details> &d, const pk::Error &e) {
      std::string t = p.name + " " + p.version + " (" + p.arch + ")\n" + p.summary + "\n\n";
      for (auto &x : d) {
        if (x.description != p.summary && x.description != x.summary) t += x.description + "\n\n";
        if (!x.url.empty()) t += "Website: " + x.url + "\n";
        if (!x.license.empty()) t += "License: " + x.license + "\n";
        if (x.size) t += "Size: " + fld::human_size(x.size) + "\n";
        if (x.download_size && !p.installed()) t += "Download: " + fld::human_size(x.download_size) + "\n";
        t += "Group: " + pk::group_text(x.group) + "\n";
      }
      if (!p.repo.empty()) t += "Repository: " + p.repo + "\n";
      if (e) t += e.text;
      sw_buf->text(t.c_str());
    });
  }

  void install_or_remove() {
    int i = sw_list->value() - 1;
    if (i < 0 || i >= (int)results.size()) return;
    pk::Package p = results[i];
    if (p.installed()) {
      if (fl_choice("Remove %s?", "Cancel", "Remove", nullptr, p.name.c_str()) != 1) return;
      apply(Action::REMOVE, {p.id}, "Removing " + p.name, [this, p](bool ok) {
        if (ok) notify_done(p.name + " was removed.");
        refresh_after_change();
      });
    } else {
      apply(Action::INSTALL, {p.id}, "Installing " + p.name, [this, p](bool ok) {
        if (ok) notify_done(p.name + " was installed.");
        refresh_after_change();
      });
    }
  }

  void notify_done(const std::string &body) {
    fld::Notification n;
    n.summary = "Software";
    n.body = body;
    n.icon = "fl-packages";
    n.transient = true;
    fld::notify(n);
  }

  void refresh_after_change() {
    if (sw_groups->value() > 0) browse_group();
    else if (!fld::trim(sw_search->value()).empty()) do_search();
    load_history();
    check_updates(false);
  }

  // --- repositories
  void load_repos() {
    pk::get_repos([this](const std::vector<pk::Repo> &list, const pk::Error &e) {
      repos = list;
      repo_list->clear();
      for (auto &r : repos) {
        std::string line = r.id + (r.description.empty() || r.description == r.id ? "" : "  — " + r.description);
        repo_list->add(line.c_str(), r.enabled);
      }
      if (e) set_status(e.text);
    });
  }

  void repo_toggled() {
    int i = repo_list->value() - 1;
    if (i < 0 || i >= (int)repos.size()) return;
    bool on = repo_list->checked(i + 1);
    if (on == repos[i].enabled) return;
    std::string id = repos[i].id;
    set_status(std::string(on ? "Enabling " : "Disabling ") + id + "…");
    pk::set_repo_enabled(id, on, [this](const pk::Error &e) {
      if (e && e.code != -1) fl_alert("The repository could not be changed.\n\n%s", e.text.c_str());
      set_status("");
      load_repos();
    });
  }

  // --- history
  void load_history() {
    pk::get_history(200, [this](const std::vector<pk::PastTransaction> &list, const pk::Error &e) {
      hist_list->clear();
      int shown = 0;
      // a dependency check (simulation) is logged like the change it came
      // before: skip it when the next newer entry changed the same packages
      auto pkgs = [](const pk::PastTransaction &t) {
        std::set<std::string> s;
        for (auto &l : pk::history_lines(t.data))
          if (!fld::starts_with(l, "Downloading")) s.insert(l);
        return s;
      };
      const pk::PastTransaction *newer = nullptr;
      for (auto &t : list) {
        if (newer && newer->role == t.role && pkgs(*newer) == pkgs(t) && newer->when.substr(0, 15) == t.when.substr(0, 15)) {
          newer = &t;
          continue;
        }
        newer = &t;
        if (t.role == "get-updates" || t.role == "resolve" || t.role == "get-details" || t.role == "search-name" ||
            t.role == "get-packages" || t.role == "search-details" || t.role == "get-update-detail" ||
            t.role == "get-repo-list" || t.role == "search-group" || t.role == "get-depends")
          continue;
        std::string when = t.when.substr(0, 16);
        std::replace(when.begin(), when.end(), 'T', ' ');
        hist_list->add((when + "   " + pk::role_text(t.role) + (t.succeeded ? "" : "  (failed)")).c_str());
        for (auto &l : pk::history_lines(t.data)) hist_list->add(("        " + l).c_str());
        if (++shown >= 60) break;
      }
      if (!shown) hist_list->add(e ? e.text.c_str() : "No changes yet.");
    });
  }

  void tab_changed() {
    Fl_Widget *v = tabs->value();
    if (v == sw_tab) ensure_catalog();
    if (v == repo_tab && repos.empty()) load_repos();
    if (v == hist_tab && !hist_list->size()) load_history();
    if (v == sw_tab) sw_search->take_focus();
  }
};

static void ensure_catalog() {
#ifdef HAVE_APPSTREAM
  static bool started = false;
  if (started) return;
  started = true;
  std::thread([] {
    auto *apps = new std::map<std::string, appicons::AppInfo>(appicons::load_catalog());
    // the pool is freed: give its memory back to the system
    malloc_trim(0);
    Fl::awake([](void *d) {
      auto *a = static_cast<std::map<std::string, appicons::AppInfo> *>(d);
      g_apps.swap(*a);
      delete a;
      g_apps_ready = true;
      if (g_main_win) g_main_win->refresh_icons();
    }, apps);
  }).detach();
#endif
}

// ---- background update checker ----------------------------------------------------------

std::string updates_ini() { return fld::fld_config_dir() + "/updates.ini"; }

// One check: refresh the cache when it is old, and announce new updates.
int check_updates_once() {
  std::string last_notified;
  {
    std::string s;
    fld::read_file(fld::cache_home() + "/fldesktop/updates-notified", s);
    last_notified = fld::trim(s);
  }
  // (a loop of one round, so that `continue` ends the check)
  for (int round = 0; round < 1; round++) {
    fld::Ini ini;
    ini.load(updates_ini());
    std::string freq = fld::lower(ini.get("Updates", "Check", "daily"));
    bool notify = ini.get_bool("Updates", "Notify", true);
    unsigned interval = freq == "weekly" ? 7 * 86400 : freq == "hourly" ? 3600 : 86400;
    if (freq == "never") continue;
    if (!pk::available()) continue;
    bool done = false;
    unsigned since = 0;
    pk::time_since_refresh([&](unsigned s) { since = s; done = true; });
    while (!done) Fl::wait(0.5);
    if (since >= interval) {
      done = false;
      pk::refresh_cache(false, [&](const pk::Error &) { done = true; });
      while (!done) Fl::wait(0.5);
    }
    std::vector<pk::Package> ups;
    done = false;
    pk::get_updates([&](const std::vector<pk::Package> &l, const pk::Error &) { ups = l; done = true; });
    while (!done) Fl::wait(0.5);
    if (ups.empty() || !notify) continue;
    std::string key;
    int security = 0;
    for (auto &p : ups) {
      key += p.id + ",";
      if (p.info == 9) security++;
    }
    gchar *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key.c_str(), -1);
    std::string h = sum;
    g_free(sum);
    if (h == last_notified) continue; // the same updates were announced already
    last_notified = h;
    fld::mkdir_p(fld::cache_home() + "/fldesktop");
    fld::write_file(fld::cache_home() + "/fldesktop/updates-notified", h + "\n");
    fld::Notification n;
    n.app_name = "Software Packages";
    n.desktop_entry = "fl-packages";
    n.icon = "fl-packages";
    n.summary = std::to_string(ups.size()) + (ups.size() == 1 ? " update is available" : " updates are available");
    n.body = security ? std::to_string(security) + (security == 1 ? " of them fixes" : " of them fix") +
                            " security problems."
                      : "Install them with Software Packages.";
    n.urgency = security ? 2 : 1;
    n.actions = {{"default", "Show Updates"}};
    // the notification's button works only while this runs: wait a while
    // for it (the notification stays in the list after that)
    bool clicked = false;
    fld::notify(n, [&clicked](const std::string &) {
      clicked = true;
      fld::spawn({"fl-packages", "--updates"});
    });
    for (int i = 0; i < 600 && !clicked; i++) Fl::wait(1.0);
  }
  return 0;
}

// The background checker sleeps between checks without any of the libraries
// set up (still a few MB: the binary maps FLTK and the rest) and runs each
// check as `fl-packages --check-updates --once`: first a minute after login
// (at once with --now), then every few hours. It ends with the session: once
// a minute it checks that the compositor still answers.
int check_updates_scheduler(bool now) {
  const int slice = 60;
  int wait = now ? 0 : 60;
  for (;;) {
    while (wait > 0) {
      int step = std::min(wait, slice);
      struct timespec ts = {(time_t)step, 0};
      while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
      wait -= step;
      if (!fld::wayland_display_alive()) return 0;
    }
    if (!fld::wayland_display_alive()) return 0;
    pid_t pid = fork();
    if (pid == 0) {
      execl("/proc/self/exe", "fl-packages", "--check-updates", "--once", (char *)nullptr);
      execlp("fl-packages", "fl-packages", "--check-updates", "--once", (char *)nullptr);
      _exit(127);
    }
    if (pid > 0) {
      int st;
      while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    }
    wait = 6 * 3600;
  }
}

} // namespace

int main(int argc, char **argv) {
  bool check = false, now = false, updates_first = false, once = false;
  std::string search, show, show_tab;
  std::vector<std::string> files;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-packages [--updates | --search TERM | --show NAME [--tab files|depends|required-by] |\n"
             "                   FILE... | --check-updates [--now]]\n"
             "Installs, removes and updates software through PackageKit.\n");
      return 0;
    } else if (a == "--check-updates") check = true;
    else if (a == "--once") once = true;
    else if (a == "--now") now = true;
    else if (a == "--updates") updates_first = true;
    else if (a == "--search" && i + 1 < argc) search = argv[++i];
    else if (a == "--show" && i + 1 < argc) show = argv[++i];
    else if (a == "--tab" && i + 1 < argc) show_tab = argv[++i];
    else if (fld::starts_with(a, "file://")) files.push_back(fld::uri_decode(a.substr(7)));
    else files.push_back(fld::abs_path(a));
  }
  if (check && !once) return check_updates_scheduler(now);
  fld::init_app("fl-packages", argc, argv);
  Fl_Image::RGB_scaling(FL_RGB_SCALING_BILINEAR); // smooth scaled app icons
  Fl::lock();
  if (check) return check_updates_once();
  if (!pk::available()) {
    fl_alert("The PackageKit service is not available.\n\nInstall PackageKit (packagekit) to manage software here.");
    return 1;
  }
  if (!files.empty()) {
    bool done = false, ok = false;
    std::string names;
    for (auto &f : files) names += (names.empty() ? "" : ", ") + fld::basename(f);
    apply(Action::FILES, files, "Installing " + names, [&](bool r) {
      ok = r;
      done = true;
    });
    while (!done) Fl::wait();
    if (ok) fl_message("%s installed.", names.c_str());
    return ok ? 0 : 1;
  }
  MainWindow *w = new MainWindow;
  g_main_win = w;
  w->show(); // not show(argc, argv): it resets the theme colors
  w->set_status(pk::backend_name());
  w->check_updates(false);
  if (!show.empty()) {
    // a package's details: search its name and select it
    w->tabs->value(w->sw_tab);
    w->pending_select = show;
    w->sw_search->value(show.c_str());
    w->do_search();
    if (show_tab == "files") w->info_tabs->value(w->tab_files);
    else if (show_tab == "depends") w->info_tabs->value(w->tab_deps);
    else if (show_tab == "required-by") w->info_tabs->value(w->tab_rdeps);
  } else if (!search.empty()) {
    w->tabs->value(w->sw_tab);
    w->sw_search->value(search.c_str());
    w->do_search();
  } else if (updates_first) {
    w->tabs->value(w->upd_tab);
  }
  return Fl::run();
}
