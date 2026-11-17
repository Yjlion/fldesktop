// fl-flatpak: Flatpak applications for fldesktop (Flathub and other sources)
//
//   fl-flatpak [APP-ID]          the store (showing the app)
//   fl-flatpak FILE              install a .flatpakref / .flatpakrepo / .flatpak
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_ask.H>

#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "fld/icons.h"
#include "fld/notify.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fpclient.h"
#include "permissions.h"

namespace {

// icons of catalog apps are files; keep scaled copies
std::map<std::string, Fl_Image *> g_icons;
Fl_Image *app_icon(const fp::App &a, int size) {
  std::string key = a.icon + "@" + std::to_string(size);
  auto it = g_icons.find(key);
  if (it != g_icons.end()) return it->second;
  Fl_Image *img = nullptr;
  if (!a.icon.empty()) {
    if (Fl_Image *raw = fld::load_image(a.icon)) {
      img = raw->copy(size, size);
      delete raw;
    }
  }
  if (!img) img = fld::icon_first({a.id, "application-x-executable"}, size);
  g_icons[key] = img;
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
    auto *b = new Fl_Check_Button(28, y, 480, 24);
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
  Fl_Hold_Browser *nav, *list;
  Fl_Input *search;
  Fl_Choice *target;
  Fl_Box *banner, *icon, *title, *meta, *status;
  Fl_Button *add_flathub, *action, *remove, *run, *perms, *update_all;
  Fl_Text_Display *desc;
  Fl_Text_Buffer *buf;
  std::string title_s, meta_s, select_id;
  enum { EXPLORE = 1, INSTALLED, UPDATES, FIRST_CATEGORY } ;

  StoreWindow() : Fl_Double_Window(1000, 620, "Flatpak Apps") {
    const int W = w(), H = h();
    search = new Fl_Input(12, 10, 300, 30);
    search->when(FL_WHEN_CHANGED);
    search->callback([](Fl_Widget *, void *d) { ((StoreWindow *)d)->filter(); }, this);
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
    list = new Fl_Hold_Browser(220, 50, 380, H - 86);
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
    desc = new Fl_Text_Display(dx, 204, dw, H - 240);
    buf = new Fl_Text_Buffer;
    desc->buffer(buf);
    desc->wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    desc->box(FL_FLAT_BOX);
    desc->color(fld::palette().bg);
    status = new Fl_Box(12, H - 32, W - 24, 28);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    status->labelcolor(fld::palette().fg_dim);
    end();
    resizable(list);
    size_range(820, 520);
    fld::set_window_icon(this, "fl-flatpak");
    show_detail(nullptr);
  }

  void set_status(const std::string &s) {
    status->copy_label(fld::escape_at(s).c_str());
    status->redraw();
  }

  void reload(bool refresh) {
    set_status(refresh ? "Updating the catalogs…" : "Loading…");
    fp::load_catalog(refresh, [this](const fp::Catalog &c) {
      catalog = c;
      fill_nav();
      filter();
      if (!c.error.empty()) set_status(c.error);
      else {
        int inst = 0;
        for (auto &a : catalog.apps) inst += a.installed;
        set_status(std::to_string(catalog.apps.size()) + " apps, " + std::to_string(inst) + " installed · sources: " +
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
        for (size_t i = 0; i < shown.size(); i++)
          if (catalog.apps[shown[i]].id == select_id) {
            list->value((int)i + 1);
            app_selected();
          }
        select_id.clear();
      }
    }, [this](const fp::Progress &p) { set_status(p.status); });
  }

  void fill_nav() {
    int keep = std::max(1, nav->value());
    nav->clear();
    int updates = 0, installed = 0;
    for (auto &a : catalog.apps) {
      updates += a.update;
      installed += a.installed;
    }
    nav->add("Explore");
    nav->icon(1, fld::icon_first({"fl-flatpak", "system-software-install"}, 20));
    nav->add(("Installed (" + std::to_string(installed) + ")").c_str());
    nav->add(updates ? ("Updates (" + std::to_string(updates) + ")").c_str() : "Updates");
    for (auto &c : fp::browse_categories()) nav->add(("   " + fp::category_label(c)).c_str());
    nav->value(keep);
    updates ? update_all->show() : update_all->hide();
  }

  void filter() {
    int sel = nav->value();
    std::string q = fld::lower(fld::trim(search->value()));
    shown.clear();
    list->clear();
    for (size_t i = 0; i < catalog.apps.size(); i++) {
      const fp::App &a = catalog.apps[i];
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
      std::string label = a.name + (a.update ? "  · update available" : a.installed ? "  · installed" : "");
      list->add(label.c_str());
      list->icon(list->size(), app_icon(a, 32));
    }
    if (shown.empty()) {
      list->add(catalog.apps.empty() ? "No apps: add a source such as Flathub." : "Nothing found.");
      show_detail(nullptr);
    }
    list->redraw();
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
    if (a->installed) {
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
  std::string select, file;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-flatpak [APP-ID | FILE]\nFinds, installs and updates Flatpak apps; FILE is a .flatpakref,\n"
             ".flatpakrepo or .flatpak bundle.\n");
      return 0;
    }
    if (fld::starts_with(a, "file://")) a = fld::uri_decode(a.substr(7));
    std::string ext = fld::extension(a);
    if (ext == "flatpakref" || ext == "flatpakrepo" || ext == "flatpak") file = fld::abs_path(a);
    else select = a;
  }
  fld::init_app("fl-flatpak", argc, argv);
  Fl::lock();
  StoreWindow *w = new StoreWindow;
  w->select_id = select;
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
