// fl-flatpak: Flatpak applications for fldesktop (Flathub and other sources)
//
//   fl-flatpak [APP-ID]          the store (showing the app)
//   fl-flatpak FILE              install a .flatpakref / .flatpakrepo / .flatpak
#include <FL/Fl.H>
#include <FL/Fl_Browser.H>
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_ask.H>

#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

#include <algorithm>
#include <functional>
#include <set>
#include <thread>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fld/menus.h"
#include "fld/icons.h"
#include "fld/notify.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fpclient.h"
#include "permissions.h"

namespace {

// icons of catalog apps are files; keep scaled copies. Apps with only a
// remote icon get it downloaded (curl) into ~/.cache/fldesktop/flatpak-icons.
std::map<std::string, Fl_Image *> g_icons;
std::set<std::string> g_downloading;
void (*g_icons_changed)() = nullptr;

std::string icon_cache_file(const std::string &url) {
  uint64_t h = 1469598103934665603ULL;
  for (unsigned char c : url) {
    h ^= c;
    h *= 1099511628211ULL;
  }
  char b[40];
  snprintf(b, sizeof b, "%016llx", (unsigned long long)h);
  std::string ext = url.size() > 4 && url.compare(url.size() - 4, 4, ".svg") == 0 ? ".svg" : ".png";
  return fld::cache_home() + "/fldesktop/flatpak-icons/" + b + ext;
}

void icons_changed_cb(void *) {
  if (g_icons_changed) g_icons_changed();
}

void fetch_remote_image(const std::string &url) {
  if (url.compare(0, 8, "https://") != 0 || g_downloading.count(url) || fld::which("curl").empty()) return;
  g_downloading.insert(url);
  std::string dest = icon_cache_file(url);
  fld::mkdir_p(fld::dirname(dest));
  std::thread([url, dest] {
    std::string out;
    std::string tmp = dest + ".part";
    int rc = fld::run_capture({"curl", "-fsSL", "--max-time", "20", "--max-filesize", "2000000", "-o", tmp, url}, out);
    if (rc == 0) rename(tmp.c_str(), dest.c_str());
    else unlink(tmp.c_str());
    Fl::awake([](void *) {
      // a short delay gathers several finished downloads into one redraw
      Fl::remove_timeout(icons_changed_cb);
      Fl::add_timeout(0.3, icons_changed_cb);
    }, nullptr);
  }).detach();
}

// Rows get this until they are first drawn: the same height as a real icon,
// so the list does not move when icons are loaded.
Fl_Image *placeholder_icon() {
  static uchar pixels[32 * 32 * 4] = {};
  static Fl_RGB_Image img(pixels, 32, 32, 4);
  return &img;
}

// The app list. The catalog has thousands of apps: loading all their icons
// (and downloading the missing ones) took seconds and a lot of memory, so
// only the lines that are drawn get their icon.
class AppList : public Fl_Hold_Browser {
public:
  std::function<Fl_Image *(int line)> icon_for; // line from 1; nullptr = none
  AppList(int X, int Y, int W, int H) : Fl_Hold_Browser(X, Y, W, H) {}
  // after the lines changed, or when better icons may be available
  void reset_icons() {
    loaded_.assign((size_t)size() + 1, 0);
    redraw();
  }
  void draw() override {
    // the browser finds its top line while drawing: draw, then load the
    // icons of the lines now on screen and draw again with them
    Fl_Hold_Browser::draw();
    if (!icon_for) return;
    if ((int)loaded_.size() < size() + 1) loaded_.resize((size_t)size() + 1, 0);
    bool any = false;
    for (int l = std::max(1, topline()); l <= size() && displayed(l); l++) {
      if (loaded_[l]) continue;
      loaded_[l] = 1;
      if (Fl_Image *img = icon_for(l)) {
        icon(l, img);
        any = true;
      }
    }
    if (any) {
      clear_damage(FL_DAMAGE_ALL);
      Fl_Hold_Browser::draw();
    }
  }

private:
  std::vector<uint8_t> loaded_;
};

Fl_Image *app_icon(const fp::App &a, int size) {
  std::string key = a.id + "|" + a.icon + "@" + std::to_string(size);
  auto it = g_icons.find(key);
  if (it != g_icons.end()) return it->second;
  Fl_Image *img = nullptr;
  std::string file = a.icon;
  bool pending = false;
  if (file.empty() && !a.icon_url.empty()) {
    std::string cached = icon_cache_file(a.icon_url);
    if (fld::file_exists(cached)) file = cached;
    else if (fld::icon_path(a.icon_stock.empty() ? a.id : a.icon_stock, size).empty()) {
      fetch_remote_image(a.icon_url);
      pending = g_downloading.count(a.icon_url) > 0;
    }
  }
  if (!file.empty()) {
    if (Fl_Image *raw = fld::load_image(file)) {
      if (raw->w() > 0 && raw->h() > 0) img = raw->copy(size, size);
      delete raw;
    }
  }
  if (!img && !a.icon_stock.empty()) img = fld::icon(a.icon_stock, size);
  if (!img) img = fld::icon_first({a.id, a.runtime ? "application-x-addon" : "application-x-executable"}, size);
  // a download is on its way: do not remember the placeholder
  if (!pending) g_icons[key] = img;
  return img;
}

// ---- progress ----------------------------------------------------------------------------

class ProgressWindow : public Fl_Double_Window {
public:
  Fl_Box *status;
  Fl_Progress *bar;
  fp::Job job;
  ProgressWindow(const std::string &title) : Fl_Double_Window(440, 130) {
    copy_label(title.c_str());
    status = new Fl_Box(16, 14, 408, 24, "Starting");
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    bar = new Fl_Progress(16, 46, 408, 22);
    bar->maximum(100);
    bar->selection_color(fld::palette().accent);
    Fl_Button *c = new Fl_Button(324, 86, 100, 30, "Cancel");
    c->callback([](Fl_Widget *w, void *) {
      ((ProgressWindow *)w->window())->job.cancel();
      w->deactivate();
    });
    end();
    set_modal();
    callback([](Fl_Widget *, void *) {});
    fld::set_window_icon(this, "fl-flatpak");
  }
  void update(const fp::Progress &p) {
    status->copy_label(fld::escape_at(p.status).c_str());
    if (p.percent >= 0) bar->value((float)p.percent);
    redraw();
  }
};

// runs a job with a progress window; done(ok) afterwards
void with_progress(const std::string &title, std::function<fp::Job(fp::ProgressFn, fp::Done)> start,
                   std::function<void(bool)> done) {
  auto win = std::make_shared<ProgressWindow *>(new ProgressWindow(title));
  (*win)->show();
  (*win)->job = start([win](const fp::Progress &p) {
    if (*win) (*win)->update(p);
  }, [win, title, done](const std::string &e) {
    if (*win) {
      (*win)->hide();
      Fl::delete_widget(*win);
      *win = nullptr;
    }
    if (!e.empty() && e != "Canceled.") fl_alert("%s failed.\n\n%s", title.c_str(), e.c_str());
    if (done) done(e.empty());
  });
}

// ---- permissions dialog --------------------------------------------------------------------

void permissions_dialog(const fp::App &app) {
  std::string meta = fp::metadata(app);
  if (meta.empty()) {
    fl_alert("The permissions of %s cannot be read.", app.name.c_str());
    return;
  }
  auto perms = std::make_shared<fp::Permissions>();
  fp::parse_metadata(meta, *perms);
  std::string path = fp::overrides_path(app.id), existing;
  fld::read_file(path, existing);
  fp::parse_overrides(existing, *perms);

  Fl_Double_Window w(560, 620);
  std::string title = "Permissions of " + app.name;
  w.copy_label(title.c_str());
  Fl_Box *note = new Fl_Box(16, 8, 528, 40,
                            "Changes apply the next time the app starts. Bold entries differ from what the app "
                            "asks for.");
  note->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
  note->labelsize(12);
  note->labelcolor(fld::palette().fg_dim);
  Fl_Scroll *sc = new Fl_Scroll(8, 52, 544, 510);
  sc->type(Fl_Scroll::VERTICAL);
  struct Row {
    Fl_Check_Button *b;
    fp::PermItem item;
  };
  auto rows = std::make_shared<std::vector<Row>>();
  int y = 56;
  std::string cat;
  static const std::map<std::string, std::string> heads = {
      {"shared", "Network and communication"}, {"sockets", "Services"}, {"devices", "Devices"},
      {"features", "Features"}};
  auto refresh_rows = [rows, perms] {
    for (auto &r : *rows) {
      r.b->value(perms->effective(r.item.category, r.item.key));
      r.b->labelfont(perms->overridden(r.item.category, r.item.key) ? FL_HELVETICA_BOLD : FL_HELVETICA);
      r.b->redraw();
    }
  };
  for (auto &it : fp::perm_items()) {
    if (it.category != cat) {
      cat = it.category;
      Fl_Box *h = new Fl_Box(16, y + 4, 500, 22);
      h->copy_label(heads.at(cat).c_str());
      h->labelfont(FL_HELVETICA_BOLD);
      h->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
      y += 28;
    }
    auto *b = new fld::CheckButton(28, y, 480, 24);
    b->copy_label(it.label.c_str());
    rows->push_back({b, it});
    y += 26;
  }
  Fl_Box *fh = new Fl_Box(16, y + 6, 500, 22, "Files and folders");
  fh->labelfont(FL_HELVETICA_BOLD);
  fh->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  y += 32;
  Fl_Hold_Browser *fs = new Fl_Hold_Browser(28, y, 360, 120);
  fs->format_char(0);
  fs->textsize(13);
  auto fill_fs = [fs, perms] {
    fs->clear();
    for (auto &f : perms->filesystems()) fs->add(fp::filesystem_label(f).c_str(), (void *)new std::string(f));
  };
  Fl_Button *add = new Fl_Button(396, y, 140, 28, "Add…");
  Fl_Button *rm = new Fl_Button(396, y + 34, 140, 28, "Remove");
  y += 128;
  sc->end();
  for (auto &r : *rows) {
    r.b->callback([](Fl_Widget *w, void *d) {
      auto *ctx = (std::pair<std::shared_ptr<fp::Permissions>, fp::PermItem> *)d;
      ctx->first->set(ctx->second.category, ctx->second.key, ((Fl_Check_Button *)w)->value());
      w->labelfont(ctx->first->overridden(ctx->second.category, ctx->second.key) ? FL_HELVETICA_BOLD : FL_HELVETICA);
      w->redraw();
    }, new std::pair<std::shared_ptr<fp::Permissions>, fp::PermItem>(perms, r.item));
  }
  static std::function<void()> s_fill;
  s_fill = fill_fs;
  add->callback([](Fl_Widget *, void *d) {
    auto perms = *(std::shared_ptr<fp::Permissions> *)d;
    const char *v = fl_input("Folder to allow (a path, ~/Folder, or home, host, xdg-download...).\n"
                             "Add :ro for read only access.", "xdg-download");
    if (!v || !*fld::trim(v).c_str()) return;
    perms->add_filesystem(fld::trim(v));
    s_fill();
  }, new std::shared_ptr<fp::Permissions>(perms));
  rm->callback([](Fl_Widget *, void *d) {
    auto *ctx = (std::pair<std::shared_ptr<fp::Permissions>, Fl_Hold_Browser *> *)d;
    int i = ctx->second->value();
    if (i < 1) return;
    std::string f = *(std::string *)ctx->second->data(i);
    ctx->first->remove_filesystem(f);
    s_fill();
  }, new std::pair<std::shared_ptr<fp::Permissions>, Fl_Hold_Browser *>(perms, fs));
  Fl_Button *reset = new Fl_Button(16, 576, 150, 30, "Reset to Defaults");
  Fl_Button *cancel = new Fl_Button(336, 576, 100, 30, "Cancel");
  Fl_Return_Button *save = new Fl_Return_Button(444, 576, 100, 30, "Save");
  static int result;
  result = 0;
  static std::function<void()> s_refresh;
  s_refresh = refresh_rows;
  reset->callback([](Fl_Widget *, void *d) {
    (*(std::shared_ptr<fp::Permissions> *)d)->reset();
    s_refresh();
    s_fill();
  }, new std::shared_ptr<fp::Permissions>(perms));
  cancel->callback([](Fl_Widget *b, void *) { result = 0; b->window()->hide(); });
  save->callback([](Fl_Widget *b, void *) { result = 1; b->window()->hide(); });
  w.end();
  w.resizable(sc);
  w.set_modal();
  fld::set_window_icon(&w, "fl-flatpak");
  refresh_rows();
  fill_fs();
  w.show();
  while (w.shown()) Fl::wait();
  if (result != 1) return;
  std::string text = fp::write_overrides(existing, *perms);
  fld::mkdir_p(fld::dirname(path));
  if (fld::trim(text).empty()) unlink(path.c_str());
  else if (!fld::write_file(path, text)) fl_alert("The permissions could not be saved to %s.", path.c_str());
}

// ---- main window ------------------------------------------------------------------------------

class StoreWindow : public Fl_Double_Window {
public:
  fp::Catalog catalog;
  std::vector<int> shown; // indexes into catalog.apps
  Fl_Hold_Browser *nav;
  AppList *list;
  Fl_Input *search;
  Fl_Choice *target;
  Fl_Box *banner, *icon, *title, *meta, *status;
  Fl_Button *add_flathub, *action, *remove, *run, *perms, *update_all;
  Fl_Text_Display *desc;
  Fl_Text_Buffer *buf;
  std::string title_s, meta_s, select_id, select_tab;
  enum { EXPLORE = 1, INSTALLED, UPDATES, RUNTIMES, FIRST_CATEGORY } ;
  // detail tabs
  Fl_Tabs *tabs;
  Fl_Box *empty;
  Fl_Group *tab_about, *tab_files, *tab_deps, *tab_used;
  Fl_Input *files_filter;
  Fl_Browser *files_list, *deps_list, *used_list;
  std::string info_ref;          // ref the tabs show ("" = none)
  bool info_loaded = false, info_loading = false;
  fp::RefInfo info;

  StoreWindow() : Fl_Double_Window(1000, 620, "Flatpak Apps") {
    const int W = w(), H = h();
    search = new Fl_Input(12, 10, 300, 30);
    search->when(FL_WHEN_CHANGED);
    search->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->filter(); }, this);
    static StoreWindow *self;
    self = this;
    g_icons_changed = [] { self->refresh_icons(); };
    search->tooltip("Search apps by name, summary or id");
    target = new Fl_Choice(W - 230, 10, 218, 30, "Install for:");
    target->add("All users");
    target->add("Only me");
    target->value(0);
    update_all = new Fl_Button(W - 400, 10, 150, 30, "Update All");
    update_all->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->do_update_all(); }, this);
    update_all->hide();
    banner = new Fl_Box(330, 10, 300, 30);
    banner->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    add_flathub = new Fl_Button(W - 400, 10, 150, 30, "Add Flathub");
    add_flathub->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->do_add_flathub(); }, this);
    add_flathub->hide();
    nav = new Fl_Hold_Browser(12, 50, 200, H - 86);
    nav->textsize(14);
    nav->format_char(0);
    nav->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->filter(); }, this);
    list = new AppList(220, 50, 380, H - 86);
    list->icon_for = [this](int line) -> Fl_Image * {
      if (line < 1 || line > (int)shown.size()) return nullptr;
      return app_icon(catalog.apps[(size_t)shown[line - 1]], 32);
    };
    list->format_char(0);
    list->textsize(14);
    list->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->app_selected(); }, this);
    int dx = 612, dw = W - dx - 12;
    icon = new Fl_Box(dx, 50, 64, 64);
    title = new Fl_Box(dx + 76, 50, dw - 76, 30);
    title->labelfont(FL_HELVETICA_BOLD);
    title->labelsize(FL_NORMAL_SIZE + 4);
    title->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    meta = new Fl_Box(dx + 76, 80, dw - 76, 40);
    meta->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
    meta->labelcolor(fld::palette().fg_dim);
    meta->labelsize(12);
    action = new Fl_Button(dx, 130, 110, 30, "Install");
    action->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->do_action(); }, this);
    run = new Fl_Button(dx + 116, 130, 80, 30, "Open");
    run->callback([](Fl_Widget *, void *d) {
      StoreWindow *s = (StoreWindow *)d;
      if (const fp::App *a = s->current()) fp::launch(*a, [](const std::string &e) {
        if (!e.empty()) fl_alert("%s", e.c_str());
      });
    }, this);
    remove = new Fl_Button(dx + 202, 130, 90, 30, "Remove");
    remove->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->do_remove(); }, this);
    perms = new Fl_Button(dx, 166, 160, 30, "Permissions…");
    perms->callback([](Fl_Widget *, void *d) {
      StoreWindow *s = (StoreWindow *)d;
      if (const fp::App *a = s->current()) permissions_dialog(*a);
    }, this);
    tabs = new fld::Tabs(dx, 204, dw, H - 240);
    {
      int ty = 204 + 26, th = H - 240 - 26;
      tab_about = new Fl_Group(dx, ty, dw, th, "About");
      desc = new Fl_Text_Display(dx, ty, dw, th);
      buf = new Fl_Text_Buffer;
      desc->buffer(buf);
      desc->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
      desc->box(FL_FLAT_BOX);
      desc->color(fld::palette().bg);
      tab_about->resizable(desc);
      tab_about->end();
      tab_files = new Fl_Group(dx, ty, dw, th, "Files");
      files_filter = new Fl_Input(dx + 4, ty + 4, dw - 8, 26);
      files_filter->tooltip("Show only the files whose path contains this text");
      files_filter->when(FL_WHEN_CHANGED);
      files_filter->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->show_files(); }, this);
      files_list = new Fl_Browser(dx, ty + 34, dw, th - 34);
      files_list->format_char(0);
      files_list->textfont(FL_COURIER);
      files_list->textsize(12);
      tab_files->resizable(files_list);
      tab_files->end();
      auto list_tab = [&](const char *label, Fl_Browser *&b) {
        Fl_Group *g = new Fl_Group(dx, ty, dw, th, label);
        b = new Fl_Hold_Browser(dx, ty, dw, th);
        b->format_char(0);
        b->textsize(13);
        b->tooltip("Double-click to show it");
        b->callback([](Fl_Widget *w, void *d) { ((StoreWindow *)d)->ref_clicked((Fl_Browser *)w); }, this);
        g->resizable(b);
        g->end();
        return g;
      };
      tab_deps = list_tab("Depends on", deps_list);
      tab_used = list_tab("Used by", used_list);
    }
    tabs->end();
    tabs->resizable(tab_about);
    tabs->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->tab_changed(); }, this);
    // in place of the details while no app is chosen
    empty = new Fl_Box(dx, 50, dw, H - 90, "Select an app to see its details.");
    empty->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER | FL_ALIGN_WRAP);
    empty->labelcolor(fld::palette().fg_dim);
    status = new Fl_Box(12, H - 32, W - 24, 28);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    status->labelcolor(fld::palette().fg_dim);
    end();
    resizable(list);
    size_range(820, 520);
    fld::set_window_icon(this, "fl-flatpak");
    show_detail(nullptr);
    make_context_menus();
  }

  // right-click menus: an app (the same as its buttons), and the ref lists
  void make_context_menus() {
    fld::set_context_menu(list, [this](fld::Menu &m) {
      m.button(action).button(run).button(remove).divider().button(perms);
    });
    files_list->type(FL_HOLD_BROWSER);
    fld::set_context_menu(files_list, [this](fld::Menu &m) {
      int i = files_list->value();
      std::string f = i > 0 && files_list->text(i) ? files_list->text(i) : "";
      bool ok = !f.empty() && f[0] == '/';
      m.item("Open Folder", [f] { fld::spawn({"xdg-open", fld::is_dir(f) ? f : fld::dirname(f)}); },
             ok ? 0 : FL_MENU_INACTIVE);
      m.item("Copy Path", [f] { Fl::copy(f.c_str(), (int)f.size(), 1); }, ok ? 0 : FL_MENU_INACTIVE);
    });
    for (Fl_Browser *b : {deps_list, used_list})
      fld::set_context_menu(b, [this, b](fld::Menu &m) {
        int i = b->value();
        std::string ref = i > 0 && b->text(i) ? b->text(i) : "";
        ref = ref.substr(0, ref.find(' '));
        m.item("Show", [this, b] {
          Fl::e_clicks = 1; // as a double click
          ref_clicked(b);
          Fl::e_clicks = 0;
        }, ref.empty() ? FL_MENU_INACTIVE : 0);
        m.item("Copy Ref", [ref] { Fl::copy(ref.c_str(), (int)ref.size(), 1); }, ref.empty() ? FL_MENU_INACTIVE : 0);
      });
  }

  void set_status(const std::string &s) {
    status->copy_label(fld::escape_at(s).c_str());
    status->redraw();
  }

  void reload(bool refresh) {
    set_status(refresh ? "Updating the catalogs…" : "Loading…");
    fp::load_catalog(refresh, [this](fp::Catalog &c) {
      catalog = std::move(c);
      fill_nav();
      filter();
#ifdef __GLIBC__
      malloc_trim(0); // the previous catalog
#endif
      if (!catalog.error.empty()) set_status(catalog.error);
      else {
        int inst = 0, apps = 0;
        for (auto &a : catalog.apps) {
          inst += a.installed && !a.runtime;
          apps += !a.runtime;
        }
        set_status(std::to_string(apps) + " apps, " + std::to_string(inst) + " installed · sources: " +
                   (catalog.remotes.empty() ? "none" : fld::join(catalog.remotes, ", ")));
      }
      if (!catalog.have_flathub) {
        banner->label("Flathub, the main source of apps, is not set up.");
        add_flathub->show();
      } else {
        banner->label("");
        add_flathub->hide();
      }
      if (!select_id.empty()) {
        // a runtime is in its own list
        for (auto &a : catalog.apps)
          if (a.id == select_id && a.runtime) {
            nav->value(RUNTIMES);
            filter();
            break;
          }
        for (size_t i = 0; i < shown.size(); i++)
          if (catalog.apps[shown[i]].id == select_id) {
            list->value((int)i + 1);
            app_selected();
            break;
          }
        select_id.clear();
        if (select_tab == "files") tabs->value(tab_files);
        else if (select_tab == "depends") tabs->value(tab_deps);
        else if (select_tab == "used-by") tabs->value(tab_used);
        tab_changed();
      }
    }, [this](const fp::Progress &p) { set_status(p.status); });
  }

  void fill_nav() {
    int keep = std::max(1, nav->value());
    nav->clear();
    int updates = 0, installed = 0;
    for (auto &a : catalog.apps) {
      updates += a.update;
      installed += a.installed && !a.runtime;
    }
    nav->add("Explore");
    nav->icon(1, fld::icon_first({"fl-flatpak", "system-software-install"}, 20));
    nav->add(("Installed (" + std::to_string(installed) + ")").c_str());
    nav->add(updates ? ("Updates (" + std::to_string(updates) + ")").c_str() : "Updates");
    int runtimes = 0;
    for (auto &a : catalog.apps) runtimes += a.runtime;
    nav->add(("Runtimes (" + std::to_string(runtimes) + ")").c_str());
    for (auto &c : fp::browse_categories()) nav->add(("   " + fp::category_label(c)).c_str());
    nav->value(keep);
    updates ? update_all->show() : update_all->hide();
  }

  // downloaded icons arrived: set them without rebuilding the list
  void refresh_icons() {
    list->reset_icons(); // the lines on screen ask again
    if (const fp::App *a = current()) icon->image(app_icon(*a, 64));
    redraw();
  }

  void filter() {
    int sel = nav->value();
    std::string q = fld::lower(fld::trim(search->value()));
    shown.clear();
    list->clear();
    for (size_t i = 0; i < catalog.apps.size(); i++) {
      const fp::App &a = catalog.apps[i];
      if ((sel == RUNTIMES) != a.runtime) continue;  // runtimes only in their own list
      if (sel == INSTALLED && !a.installed) continue;
      if (sel == UPDATES && !a.update) continue;
      if (sel >= FIRST_CATEGORY) {
        const std::string &cat = fp::browse_categories()[sel - FIRST_CATEGORY];
        if (std::find(a.categories.begin(), a.categories.end(), cat) == a.categories.end()) continue;
      }
      if (!q.empty() && fld::lower(a.name).find(q) == std::string::npos &&
          fld::lower(a.summary).find(q) == std::string::npos && fld::lower(a.id).find(q) == std::string::npos)
        continue;
      shown.push_back((int)i);
      std::string label = a.runtime ? a.name + "  · " + a.version
                                    : a.name + (a.update ? "  · update available" : a.installed ? "  · installed" : "");
      list->add(label.c_str());
      list->icon(list->size(), placeholder_icon());
    }
    if (shown.empty()) {
      list->add(catalog.apps.empty() ? "No apps: add a source such as Flathub." : "Nothing found.");
      show_detail(nullptr);
    }
    list->reset_icons();
  }

  const fp::App *current() const {
    int i = list->value() - 1;
    if (i < 0 || i >= (int)shown.size()) return nullptr;
    return &catalog.apps[shown[i]];
  }

  void app_selected() { show_detail(current()); }

  void show_detail(const fp::App *a) {
    for (Fl_Widget *w : {(Fl_Widget *)action, (Fl_Widget *)run, (Fl_Widget *)remove, (Fl_Widget *)perms})
      a ? w->show() : w->hide();
    // the files / dependency tabs load when shown
    info_ref = a && a->installed ? a->ref : "";
    info_loaded = info_loading = false;
    info = fp::RefInfo();
    files_list->clear();
    deps_list->clear();
    used_list->clear();
    if (a && !a->installed) {
      for (Fl_Browser *b : {files_list, deps_list, used_list}) b->add("Shown once it is installed.");
    }
    tab_changed();
    a ? tabs->show() : tabs->hide();
    a ? empty->hide() : empty->show();
    if (!a) {
      icon->image(nullptr);
      title->label("");
      meta->label("");
      buf->text("");
      redraw();
      return;
    }
    icon->image(app_icon(*a, 64));
    title_s = fld::escape_at(a->name);
    title->label(title_s.c_str());
    meta_s = a->id;
    if (!a->developer.empty()) meta_s += " · " + a->developer;
    if (!a->version.empty()) meta_s += "\nVersion " + a->version;
    if (!a->license.empty()) meta_s += " · " + a->license;
    if (a->installed) meta_s += "\nInstalled (" + std::string(a->installation == "user" ? "for you" : "for all users") +
                                (a->installed_size ? ", " + fld::human_size(a->installed_size) : "") + ")";
    meta_s = fld::escape_at(meta_s);
    meta->label(meta_s.c_str());
    if (a->runtime) {
      action->label("Installed");
      action->deactivate();
      run->deactivate();
      perms->deactivate();
      remove->activate();
    } else if (a->installed) {
      action->label(a->update ? "Update" : "Installed");
      a->update ? action->activate() : action->deactivate();
      run->activate();
      remove->activate();
      perms->activate();
    } else {
      action->label("Install");
      action->activate();
      run->deactivate();
      remove->deactivate();
      perms->deactivate();
    }
    std::string t = a->summary + "\n\n" + a->description;
    if (!a->homepage.empty()) t += "\n\nWebsite: " + a->homepage;
    if (!a->remote.empty()) t += "\nSource: " + a->remote;
    buf->text(t.c_str());
    redraw();
  }

  void tab_changed() {
    Fl_Widget *t = tabs->value();
    if (t == tab_about || info_ref.empty() || info_loaded || info_loading) return;
    info_loading = true;
    for (Fl_Browser *b : {files_list, deps_list, used_list}) {
      b->clear();
      b->add("Loading…");
    }
    const fp::App *a = current();
    if (!a) return;
    std::string ref = info_ref;
    fp::ref_info(*a, [this, ref](const fp::RefInfo &r) {
      if (ref != info_ref) return;  // another app was selected meanwhile
      info = r;
      info_loaded = true;
      info_loading = false;
      show_files();
      deps_list->clear();
      used_list->clear();
      for (auto &d : r.depends) deps_list->add(d.c_str());
      for (auto &u : r.used_by) used_list->add(u.c_str());
      if (!r.error.empty()) {
        deps_list->add(r.error.c_str());
        used_list->add(r.error.c_str());
      } else {
        if (r.depends.empty()) deps_list->add("It needs no other runtime.");
        if (r.used_by.empty()) used_list->add("Nothing installed uses it.");
      }
    });
  }

  void show_files() {
    files_list->clear();
    std::string q = fld::lower(files_filter->value());
    for (auto &f : info.files)
      if (q.empty() || fld::lower(f).find(q) != std::string::npos) files_list->add(f.c_str());
    if (info.files_truncated) files_list->add("… (only the first 20000 entries)");
    if (info_loaded && info.files.empty()) files_list->add(info.error.empty() ? "(no files)" : info.error.c_str());
  }

  // a double click on a ref selects that app or runtime
  void ref_clicked(Fl_Browser *b) {
    if (Fl::event_clicks() == 0) return;
    int i = b->value();
    if (i <= 0 || !b->text(i)) return;
    std::string ref = b->text(i);
    ref = ref.substr(0, ref.find(' '));
    for (size_t k = 0; k < catalog.apps.size(); k++)
      if (catalog.apps[k].ref == ref) {
        nav->value(catalog.apps[k].runtime ? RUNTIMES : INSTALLED);
        search->value("");
        filter();
        for (size_t j = 0; j < shown.size(); j++)
          if (shown[j] == (int)k) {
            list->value((int)j + 1);
            list->middleline((int)j + 1);
            show_detail(&catalog.apps[k]);
          }
        return;
      }
  }

  void finished(bool ok, const std::string &what, const fp::App &a) {
    if (ok) {
      fld::Notification n;
      n.summary = a.name;
      n.body = what;
      n.icon = a.icon.empty() ? "fl-flatpak" : a.icon;
      n.transient = true;
      fld::notify(n);
    }
    select_id = a.id;
    reload(false);
  }

  void do_action() {
    const fp::App *cur = current();
    if (!cur) return;
    fp::App a = *cur;
    if (a.installed && a.update) {
      with_progress("Updating " + a.name, [a](fp::ProgressFn p, fp::Done d) { return fp::update({a}, p, d); },
                    [this, a](bool ok) { finished(ok, "was updated.", a); });
    } else if (!a.installed) {
      bool user = target->value() == 1;
      with_progress("Installing " + a.name, [a, user](fp::ProgressFn p, fp::Done d) { return fp::install(a, user, p, d); },
                    [this, a](bool ok) { finished(ok, "was installed.", a); });
    }
  }

  void do_remove() {
    const fp::App *cur = current();
    if (!cur || !cur->installed) return;
    fp::App a = *cur;
    if (fl_choice("Remove %s?\nIts settings and data in ~/.var/app/%s stay.", "Cancel", "Remove", nullptr,
                  a.name.c_str(), a.id.c_str()) != 1)
      return;
    with_progress("Removing " + a.name, [a](fp::ProgressFn p, fp::Done d) { return fp::uninstall(a, p, d); },
                  [this, a](bool ok) { finished(ok, "was removed.", a); });
  }

  void do_update_all() {
    std::vector<fp::App> ups;
    for (auto &a : catalog.apps)
      if (a.update) ups.push_back(a);
    if (ups.empty()) return;
    with_progress("Updating apps", [ups](fp::ProgressFn p, fp::Done d) { return fp::update(ups, p, d); },
                  [this, n = ups.size()](bool ok) {
                    if (ok) {
                      fld::Notification note;
                      note.summary = "Apps updated";
                      note.body = std::to_string(n) + (n == 1 ? " app was updated." : " apps were updated.");
                      note.icon = "fl-flatpak";
                      note.transient = true;
                      fld::notify(note);
                    }
                    reload(false);
                  });
  }

  void do_add_flathub() {
    add_flathub->deactivate();
    set_status("Adding Flathub…");
    fp::add_flathub(target->value() == 1, [this](const std::string &e) {
      add_flathub->activate();
      if (!e.empty()) {
        fl_alert("%s", e.c_str());
        return;
      }
      reload(true);
    });
  }
};

} // namespace

int main(int argc, char **argv) {
  std::string select, file, tab;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-flatpak [APP-ID [--tab files|depends|used-by] | FILE]\nFinds, installs and updates Flatpak "
             "apps; FILE is a .flatpakref,\n.flatpakrepo or .flatpak bundle.\n");
      return 0;
    }
    if (a == "--tab" && i + 1 < argc) {
      tab = argv[++i];
      continue;
    }
    if (fld::starts_with(a, "file://")) a = fld::uri_decode(a.substr(7));
    std::string ext = fld::extension(a);
    if (ext == "flatpakref" || ext == "flatpakrepo" || ext == "flatpak") file = fld::abs_path(a);
    else select = a;
  }
  fld::init_app("fl-flatpak", argc, argv);
  Fl_Image::RGB_scaling(FL_RGB_SCALING_BILINEAR); // smooth scaled app icons
  Fl::lock();
  StoreWindow *w = new StoreWindow;
  w->select_id = select;
  w->select_tab = tab;
  w->show(); // not show(argc, argv): it resets the theme colors
  if (!file.empty()) {
    std::string ext = fld::extension(file);
    bool user = fl_choice("Install %s for all users of this computer or only for you?", "Only Me", "All Users",
                          nullptr, fld::basename(file).c_str()) == 0;
    with_progress(ext == "flatpakrepo" ? "Adding the source" : "Installing " + fld::basename(file),
                  [file, user](fp::ProgressFn p, fp::Done d) { return fp::install_file(file, user, p, d); },
                  [w](bool) { w->reload(true); });
  } else {
    w->reload(false);
  }
  return Fl::run();
}
