#include "fld/file_dialog.h"
#include "fld/menus.h"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Multi_Browser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <dirent.h>
#include <strings.h>
#include <unistd.h>
#include <fnmatch.h>
#include <sys/stat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <memory>

#include "fld/icons.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

namespace fld {

bool FileFilter::matches(const std::string &filename) const {
  if (globs.empty() && mimes.empty()) return true;
  for (auto &g : globs)
    if (fnmatch(g.c_str(), filename.c_str(), FNM_CASEFOLD) == 0) return true;
  if (!mimes.empty()) {
    std::string m = mime_from_name(filename);
    if (m.empty()) m = "application/octet-stream";
    std::vector<std::string> types = {m};
    for (auto &p : mime_parents(m)) types.push_back(p);
    for (auto &want : mimes) {
      if (want == "*" || want == "*/*") return true;
      for (auto &t : types) {
        if (t == want) return true;
        if (ends_with(want, "/*") && starts_with(t, want.substr(0, want.size() - 1))) return true;
      }
    }
  }
  return false;
}

std::vector<std::pair<std::string, std::string>> file_bookmarks() {
  std::vector<std::pair<std::string, std::string>> out;
  std::string text;
  if (!read_file(config_home() + "/gtk-3.0/bookmarks", text)) return out;
  for (auto &line : split(text, '\n')) {
    std::string l = trim(line);
    if (!starts_with(l, "file://")) continue;
    size_t sp = l.find(' ');
    std::string uri = l.substr(0, sp), label = sp == std::string::npos ? "" : trim(l.substr(sp + 1));
    std::string path = uri_decode(uri.substr(7));
    if (!is_dir(path)) continue;
    out.push_back({label.empty() ? basename(path) : label, path});
  }
  return out;
}

namespace {

// menu labels: '/' makes submenus, '&' shortcuts, '_' dividers, '@' symbols
std::string menu_escape(const std::string &l) {
  std::string esc;
  for (char c : l) {
    if (c == '/' || c == '&' || c == '_' || c == '@' || c == '\\') esc += '\\';
    esc += c;
  }
  return esc;
}

std::string g_last_folder; // remembered between dialogs of one program

struct Item {
  std::string name;
  bool dir = false;
  unsigned long long size = 0;
  time_t mtime = 0;
};

class Dialog : public Fl_Double_Window {
public:
  FileDialogOptions opt;
  std::function<void(const FileDialogResult &)> done;
  std::string cwd;
  std::vector<Item> items;
  std::vector<std::pair<std::string, std::string>> places; // label, path
  Fl_Hold_Browser *place_list;
  Fl_Browser *list;
  Fl_Input *path, *name = nullptr;
  Fl_Choice *filter = nullptr;
  Fl_Check_Button *hidden;
  Fl_Button *up, *newdir, *cancel;
  Fl_Return_Button *accept;
  std::vector<Fl_Widget *> choice_widgets;
  bool finished = false;

  Dialog(const FileDialogOptions &o, std::function<void(const FileDialogResult &)> cb)
      : Fl_Double_Window(860, 540), opt(o), done(std::move(cb)) {
    std::string t = opt.title;
    if (t.empty()) t = opt.mode == FileDialogOptions::OPEN ? (opt.directory ? "Select Folder" : "Open File")
                       : opt.mode == FileDialogOptions::SAVE ? "Save File" : "Select Folder";
    copy_label(t.c_str());
    const int W = w(), H = h(), side = 180;
    // top bar
    up = new Fl_Button(side + 12, 8, 32, 28, "@8->");
    up->tooltip("Parent folder (Alt+Up)");
    up->callback([](Fl_Widget *, void *d) {
      Dialog *dl = (Dialog *)d;
      dl->go(dirname(dl->cwd));
    }, this);
    path = new Fl_Input(side + 50, 8, W - side - 50 - 230, 28);
    path->when(FL_WHEN_ENTER_KEY_ALWAYS);
    path->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->path_entered(); }, this);
    newdir = new Fl_Button(W - 224, 8, 104, 28, "New Folder");
    newdir->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->new_folder(); }, this);
    hidden = new fld::CheckButton(W - 112, 8, 104, 28, "Hidden");
    hidden->tooltip("Show hidden files (Ctrl+H)");
    hidden->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->refresh(); }, this);
    // places
    place_list = new Fl_Hold_Browser(8, 8, side - 4, H - 60);
    place_list->textsize(13);
    place_list->format_char(0);
    place_list->callback([](Fl_Widget *w, void *d) {
      Dialog *dl = (Dialog *)d;
      int i = ((Fl_Browser *)w)->value() - 1;
      if (i >= 0 && i < (int)dl->places.size()) dl->go(dl->places[i].second);
    }, this);
    // file list
    int bottom = 48; // buttons row
    int extra = 0;
    if (opt.mode == FileDialogOptions::SAVE) extra += 36;
    if (!opt.filters.empty() || !opt.choices.empty()) extra += 36;
    int list_h = H - 44 - bottom - extra - 8;
    if (opt.multiple && opt.mode == FileDialogOptions::OPEN) list = new Fl_Multi_Browser(side + 12, 44, W - side - 20, list_h);
    else list = new Fl_Hold_Browser(side + 12, 44, W - side - 20, list_h);
    list->format_char(0);
    list->column_char('\t');
    static int widths[] = {0, 90, 150, 0};
    widths[0] = list->w() - 90 - 150 - 24;
    list->column_widths(widths);
    list->textsize(13);
    list->when(FL_WHEN_CHANGED | FL_WHEN_NOT_CHANGED | FL_WHEN_ENTER_KEY);
    list->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->list_clicked(); }, this);
    int y = 44 + list_h + 8;
    if (opt.mode == FileDialogOptions::SAVE) {
      name = new Fl_Input(side + 70, y, W - side - 78, 28, "Name:");
      name->value(opt.name.c_str());
      name->when(FL_WHEN_ENTER_KEY_ALWAYS);
      name->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->do_accept(); }, this);
      y += 36;
    }
    if (!opt.filters.empty() || !opt.choices.empty()) {
      int x = side + 12;
      if (!opt.filters.empty()) {
        filter = new Fl_Choice(x + 58, y, 260, 28, "Show:");
        for (auto &f : opt.filters) {
          filter->add(menu_escape(f.name).c_str());
        }
        int cf = opt.current_filter >= 0 && opt.current_filter < (int)opt.filters.size() ? opt.current_filter : 0;
        filter->value(cf);
        filter->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->refresh(); }, this);
        x += 58 + 260 + 16;
      }
      for (auto &c : opt.choices) {
        if (c.options.empty()) {
          auto *cb = new fld::CheckButton(x, y, 170, 28);
          cb->copy_label(c.label.c_str());
          cb->value(c.selected == "true");
          choice_widgets.push_back(cb);
          x += 176;
        } else {
          fl_font(FL_HELVETICA, FL_NORMAL_SIZE);
          int lw = (int)fl_width(c.label.c_str()) + 12;
          auto *ch = new Fl_Choice(x + lw, y, 160, 28);
          ch->copy_label(c.label.c_str());
          int sel = 0;
          for (size_t i = 0; i < c.options.size(); i++) {
            ch->add(menu_escape(c.options[i].second).c_str());
            if (c.options[i].first == c.selected) sel = (int)i;
          }
          ch->value(sel);
          choice_widgets.push_back(ch);
          x += lw + 168;
        }
      }
      y += 36;
    }
    cancel = new Fl_Button(W - 236, H - 40, 110, 30, "Cancel");
    cancel->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->finish(false); }, this);
    std::string al = opt.accept_label;
    if (al.empty()) al = opt.mode == FileDialogOptions::SAVE ? "Save" : (opt.directory || opt.mode == FileDialogOptions::SAVE_FILES) ? "Select" : "Open";
    // strip GTK mnemonics ("_Open")
    al.erase(std::remove(al.begin(), al.end(), '_'), al.end());
    accept = new Fl_Return_Button(W - 120, H - 40, 110, 30);
    accept->copy_label(al.c_str());
    accept->callback([](Fl_Widget *, void *d) { ((Dialog *)d)->do_accept(); }, this);
    if (opt.mode == FileDialogOptions::OPEN && !opt.directory) newdir->hide();
    // right-click on a file or folder
    set_context_menu(list, [this](Menu &m) {
      int i = list->value() - 1;
      bool ok = i >= 0 && i < (int)items.size();
      std::string full = ok ? (cwd == "/" ? "/" + items[(size_t)i].name : cwd + "/" + items[(size_t)i].name) : "";
      if (ok && items[(size_t)i].dir) m.item("Open Folder", [this, full] { go(full); });
      m.button(accept).divider();
      m.item("Copy Path", [full] { Fl::copy(full.c_str(), (int)full.size(), 1); }, ok ? 0 : FL_MENU_INACTIVE);
      m.button(up, "Parent Folder").button(newdir).divider().button(hidden, "Show Hidden Files");
    });
    end();
    resizable(list);
    size_range(560, 360);
    callback([](Fl_Widget *w, void *) { ((Dialog *)w)->finish(false); });
    set_window_icon(this, "folder");

    fill_places();
    std::string start = opt.folder;
    if (start.empty() || !is_dir(start)) start = g_last_folder;
    if (start.empty() || !is_dir(start)) start = home_dir();
    go(start);
  }

  int handle(int e) override {
    if (e == FL_KEYBOARD || e == FL_SHORTCUT) {
      int k = Fl::event_key();
      if (k == FL_Escape) {
        finish(false);
        return 1;
      }
      if (Fl::event_ctrl() && (k == 'h' || k == 'H')) {
        hidden->value(!hidden->value());
        refresh();
        return 1;
      }
      if (Fl::event_ctrl() && (k == 'l' || k == 'L')) {
        path->take_focus();
        path->insert_position(path->size(), 0);
        return 1;
      }
      if (Fl::event_alt() && k == FL_Up) {
        go(dirname(cwd));
        return 1;
      }
      if (k == FL_BackSpace && Fl::focus() == list) {
        go(dirname(cwd));
        return 1;
      }
    }
    return Fl_Double_Window::handle(e);
  }

  void fill_places() {
    places.clear();
    places.push_back({"Home", home_dir()});
    const char *keys[][2] = {{"DESKTOP", "Desktop"}, {"DOCUMENTS", "Documents"}, {"DOWNLOAD", "Downloads"},
                             {"MUSIC", "Music"},     {"PICTURES", "Pictures"},   {"VIDEOS", "Videos"}};
    for (auto &k : keys) {
      std::string d = user_dir(k[0]);
      if (!d.empty() && d != home_dir() && is_dir(d)) places.push_back({basename(d), d});
    }
    for (auto &b : file_bookmarks()) places.push_back(b);
    places.push_back({"File System", "/"});
    const char *icons[] = {"user-home", "user-desktop", "folder-documents", "folder-download",
                           "folder-music", "folder-pictures", "folder-videos"};
    place_list->clear();
    for (size_t i = 0; i < places.size(); i++) {
      place_list->add(places[i].first.c_str());
      std::string ic = "folder";
      if (places[i].second == "/") ic = "drive-harddisk";
      else if (i == 0) ic = icons[0];
      else {
        for (size_t k = 0; k < 6; k++)
          if (places[i].second == user_dir(keys[k][0])) ic = icons[k + 1];
      }
      place_list->icon((int)i + 1, icon_first({ic, "folder"}, 18));
    }
  }

  const FileFilter *current_filter() const {
    if (!filter || filter->value() < 0 || filter->value() >= (int)opt.filters.size()) return nullptr;
    return &opt.filters[filter->value()];
  }

  bool want_files() const {
    return !(opt.mode == FileDialogOptions::OPEN && opt.directory) && opt.mode != FileDialogOptions::SAVE_FILES;
  }

  void go(const std::string &dir) {
    std::string d = abs_path(expand_tilde(dir));
    if (!is_dir(d)) {
      fl_alert("The folder \"%s\" does not exist.", d.c_str());
      return;
    }
    if (access(d.c_str(), R_OK | X_OK) != 0) {
      fl_alert("You do not have permission to open \"%s\".", d.c_str());
      return;
    }
    cwd = d;
    path->value(tilde(cwd).c_str());
    up->activate();
    if (cwd == "/") up->deactivate();
    int sel = 0;
    for (size_t i = 0; i < places.size(); i++)
      if (places[i].second == cwd) sel = (int)i + 1;
    place_list->value(sel);
    refresh();
  }

  void refresh() {
    items.clear();
    DIR *dp = opendir(cwd.c_str());
    if (dp) {
      while (dirent *de = readdir(dp)) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        if (n[0] == '.' && !hidden->value()) continue;
        struct stat st;
        if (stat((cwd == "/" ? "/" + n : cwd + "/" + n).c_str(), &st) != 0) continue;
        Item it;
        it.name = n;
        it.dir = S_ISDIR(st.st_mode);
        it.size = (unsigned long long)st.st_size;
        it.mtime = st.st_mtime;
        if (!it.dir) {
          if (!want_files()) continue;
          const FileFilter *f = current_filter();
          if (f && !f->matches(n)) continue;
        }
        items.push_back(it);
      }
      closedir(dp);
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) {
      if (a.dir != b.dir) return a.dir;
      return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    list->clear();
    for (auto &it : items) {
      std::string line = it.name + "\t" + (it.dir ? std::string("") : human_size(it.size)) + "\t" +
                         format_time(it.mtime);
      list->add(line.c_str());
      Fl_Image *ic;
      if (it.dir) ic = icon_first({"folder"}, 16);
      else {
        std::string m = mime_from_name(it.name);
        auto names = mime_icon_names(m.empty() ? "application/octet-stream" : m);
        names.push_back("text-x-generic");
        ic = icon_first(names, 16);
      }
      list->icon(list->size(), ic);
    }
    redraw();
  }

  std::vector<int> selected() const {
    std::vector<int> s;
    for (int i = 1; i <= list->size(); i++)
      if (list->selected(i)) s.push_back(i - 1);
    return s;
  }

  void list_clicked() {
    int i = list->value() - 1;
    if (i < 0 || i >= (int)items.size()) return;
    const Item &it = items[i];
    if (Fl::event_clicks() > 0 && Fl::event() == FL_RELEASE) {
      Fl::event_clicks(0);
      if (it.dir) go(cwd == "/" ? "/" + it.name : cwd + "/" + it.name);
      else do_accept();
      return;
    }
    if (Fl::event() == FL_KEYBOARD && Fl::event_key() == FL_Enter) {
      if (it.dir) go(cwd == "/" ? "/" + it.name : cwd + "/" + it.name);
      else do_accept();
      return;
    }
    if (name && !it.dir) name->value(it.name.c_str());
  }

  void path_entered() {
    std::string p = expand_tilde(trim(path->value()));
    if (p.empty()) return;
    if (p[0] != '/') p = path_join(cwd, p);
    if (is_dir(p)) {
      go(p);
      return;
    }
    // a file name typed in the path bar
    if (want_files() && file_exists(p) && opt.mode == FileDialogOptions::OPEN) {
      finish_with({abs_path(p)});
      return;
    }
    if (opt.mode == FileDialogOptions::SAVE && is_dir(dirname(p))) {
      go(dirname(p));
      name->value(basename(p).c_str());
      do_accept();
      return;
    }
    fl_alert("\"%s\" was not found.", p.c_str());
  }

  void new_folder() {
    const char *n = fl_input("Name of the new folder:", "New Folder");
    if (!n || !*trim(n).c_str()) return;
    std::string p = path_join(cwd, trim(n));
    if (mkdir(p.c_str(), 0755) != 0) {
      fl_alert("Could not create the folder: %s", strerror(errno));
      return;
    }
    go(p);
  }

  void do_accept() {
    auto sel = selected();
    std::string join_dir = cwd == "/" ? "/" : cwd + "/";
    if (opt.mode == FileDialogOptions::SAVE) {
      std::string n = trim(name->value());
      if (n.empty()) {
        // a selected folder is entered
        if (!sel.empty() && items[sel[0]].dir) go(join_dir + items[sel[0]].name);
        return;
      }
      std::string p = n[0] == '/' ? n : join_dir + n;
      if (is_dir(p)) {
        go(p);
        name->value("");
        return;
      }
      if (file_exists(p) && fl_choice("\"%s\" already exists.\nDo you want to replace it?", "Cancel", "Replace",
                                      nullptr, basename(p).c_str()) != 1)
        return;
      finish_with({p});
      return;
    }
    if (opt.mode == FileDialogOptions::SAVE_FILES || opt.directory) {
      std::vector<std::string> out;
      for (int i : sel)
        if (items[i].dir) out.push_back(join_dir + items[i].name);
      if (out.empty()) out.push_back(cwd);
      if (!opt.multiple) out.resize(1);
      if (opt.mode == FileDialogOptions::SAVE_FILES) {
        std::vector<std::string> clash;
        for (auto &f : opt.files)
          if (file_exists(path_join(out[0], f))) clash.push_back(f);
        if (!clash.empty() &&
            fl_choice("%d of the files already exist in this folder (%s).\nDo you want to replace them?", "Cancel",
                      "Replace", nullptr, (int)clash.size(), clash[0].c_str()) != 1)
          return;
      }
      finish_with(out);
      return;
    }
    std::vector<std::string> out;
    for (int i : sel) {
      if (items[i].dir) {
        if (sel.size() == 1) {
          go(join_dir + items[i].name);
          return;
        }
        continue;
      }
      out.push_back(join_dir + items[i].name);
    }
    if (out.empty()) return;
    finish_with(out);
  }

  void finish_with(const std::vector<std::string> &paths) {
    FileDialogResult r;
    r.ok = true;
    r.paths = paths;
    r.filter = filter ? filter->value() : -1;
    for (size_t i = 0; i < opt.choices.size() && i < choice_widgets.size(); i++) {
      auto &c = opt.choices[i];
      if (c.options.empty()) r.choices.push_back({c.id, ((Fl_Check_Button *)choice_widgets[i])->value() ? "true" : "false"});
      else {
        int v = ((Fl_Choice *)choice_widgets[i])->value();
        r.choices.push_back({c.id, v >= 0 && v < (int)c.options.size() ? c.options[v].first : ""});
      }
    }
    g_last_folder = cwd;
    finish(true, &r);
  }

  void finish(bool ok, FileDialogResult *r = nullptr) {
    if (finished) return;
    finished = true;
    hide();
    FileDialogResult res;
    if (ok && r) res = *r;
    auto cb = done;
    Fl::delete_widget(this);
    if (cb) cb(res);
  }
};

} // namespace

Fl_Double_Window *show_file_dialog(const FileDialogOptions &opt, std::function<void(const FileDialogResult &)> done) {
  Dialog *d = new Dialog(opt, std::move(done));
  d->show();
  // keyboard first: the file name when saving, else the list
  if (d->name) {
    d->name->take_focus();
    d->name->insert_position(0, (int)std::string(d->name->value()).rfind('.') == -1
                                    ? d->name->size()
                                    : (int)std::string(d->name->value()).rfind('.'));
  } else {
    d->list->take_focus();
    if (d->list->size() && !d->list->value()) d->list->select(1);
  }
  return d;
}

void cancel_file_dialog(Fl_Double_Window *dialog) {
  if (Dialog *d = dynamic_cast<Dialog *>(dialog)) d->finish(false);
}

FileDialogResult run_file_dialog(const FileDialogOptions &opt) {
  FileDialogResult result;
  bool done = false;
  show_file_dialog(opt, [&](const FileDialogResult &r) {
    result = r;
    done = true;
  });
  while (!done) Fl::wait();
  return result;
}

} // namespace fld
