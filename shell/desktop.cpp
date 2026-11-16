// fl-desktop: wallpaper and desktop icons
//
// app_id "fldesktop.desktop": fl-wm keeps it below all windows and sizes it
// to the primary output. Icons are the files in the XDG desktop folder.
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/trash.h"
#include "fld/util.h"

struct Item {
  std::string path, name, icon_name;
  bool is_dir = false;
  Fl_Image *icon = nullptr;
  int x = 0, y = 0;
};

static const int CELL_W = 100, CELL_H = 96, ICON = 48;

class DesktopView : public Fl_Widget {
public:
  std::string dir;
  std::vector<Item> items;
  std::set<int> selected;
  std::string wallpaper, mode = "fill";
  Fl_Color bg_color = fl_rgb_color(0x2d, 0x3a, 0x4f);
  bool show_icons = true;
  std::unique_ptr<Fl_Image> wp_src, wp_scaled;
  int wp_w = 0, wp_h = 0;
  // rubber band
  bool banding = false;
  int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;

  DesktopView(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}

  void load_config() {
    fld::Ini cfg;
    cfg.load(fld::desktop_config_path());
    wallpaper = fld::expand_tilde(cfg.get("Desktop", "Wallpaper", ""));
    if (wallpaper.empty() || !fld::file_exists(wallpaper))
      wallpaper = fld::fld_data_dir() + "/backgrounds/default.svg";
    mode = cfg.get("Desktop", "WallpaperMode", "fill");
    std::string c = cfg.get("Desktop", "BackgroundColor", "#2d3a4f");
    if (c.size() == 7 && c[0] == '#') {
      unsigned v = (unsigned)strtoul(c.c_str() + 1, nullptr, 16);
      bg_color = fl_rgb_color((v >> 16) & 255, (v >> 8) & 255, v & 255);
    }
    show_icons = cfg.get_bool("Desktop", "ShowIcons", true);
    wp_src.reset(fld::load_image(wallpaper));
    wp_scaled.reset();
    wp_w = wp_h = 0;
    redraw();
  }

  void reload() {
    std::string prev_sel;
    items.clear();
    selected.clear();
    if (!show_icons) { redraw(); return; }
    fld::mkdir_p(dir);
    std::vector<Item> dirs, files;
    for (auto &n : fld::list_dir(dir)) {
      Item it;
      it.path = fld::path_join(dir, n);
      it.name = n;
      it.is_dir = fld::is_dir(it.path);
      if (fld::ends_with(n, ".desktop")) {
        fld::DesktopEntry e;
        if (e.load(it.path)) {
          it.name = e.name;
          it.icon = fld::icon_first({e.icon, "application-x-executable"}, ICON);
        }
      }
      if (!it.icon) {
        auto names = fld::mime_icon_names(fld::mime_type(it.path));
        if (it.is_dir) {
          std::string h = fld::home_dir();
          if (it.path == fld::user_dir("DOWNLOAD")) names.insert(names.begin(), "folder-download");
          else if (it.path == fld::user_dir("DOCUMENTS")) names.insert(names.begin(), "folder-documents");
        }
        it.icon = fld::icon_first(names, ICON);
      }
      (it.is_dir ? dirs : files).push_back(it);
    }
    // special icons first
    Item home;
    home.path = fld::home_dir();
    home.name = "Home";
    home.is_dir = true;
    home.icon = fld::icon_first({"user-home", "folder"}, ICON);
    Item trash;
    trash.path = "trash:///";
    trash.name = "Trash";
    trash.icon = fld::icon_first({fld::trash_list().empty() ? "user-trash" : "user-trash-full", "user-trash"}, ICON);
    items.push_back(home);
    items.push_back(trash);
    for (auto &d : dirs) items.push_back(d);
    for (auto &f : files) items.push_back(f);
    layout_items();
    redraw();
  }

  void layout_items() {
    int col = 0, row = 0;
    int rows = std::max(1, (h() - 16) / CELL_H);
    for (auto &it : items) {
      it.x = x() + 8 + col * CELL_W;
      it.y = y() + 8 + row * CELL_H;
      if (++row >= rows) { row = 0; col++; }
    }
  }

  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    layout_items();
  }

  void draw_wallpaper() {
    fl_color(bg_color);
    fl_rectf(x(), y(), w(), h());
    if (!wp_src) return;
    if (!wp_scaled || wp_w != w() || wp_h != h()) {
      int iw = wp_src->w(), ih = wp_src->h();
      double sx = (double)w() / iw, sy = (double)h() / ih;
      double s = mode == "fit" ? std::min(sx, sy) : mode == "center" || mode == "tile" ? 1.0 : std::max(sx, sy);
      if (mode == "stretch") {
        wp_scaled.reset(wp_src->copy(w(), h()));
      } else {
        int nw = std::max(1, (int)std::lround(iw * s)), nh = std::max(1, (int)std::lround(ih * s));
        wp_scaled.reset(wp_src->copy(nw, nh));
      }
      wp_w = w();
      wp_h = h();
    }
    Fl_Image *img = wp_scaled.get();
    if (mode == "tile") {
      for (int ty = 0; ty < h(); ty += img->h())
        for (int tx = 0; tx < w(); tx += img->w()) img->draw(x() + tx, y() + ty);
    } else {
      img->draw(x() + (w() - img->w()) / 2, y() + (h() - img->h()) / 2);
    }
  }

  static std::vector<std::string> wrap_label(const std::string &s, int width, int max_lines) {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
      int len = fl_utf8len1(s[i]);
      std::string ch = s.substr(i, len);
      if (fl_width((cur + ch).c_str()) > width && !cur.empty()) {
        // try to break at the last space
        size_t sp = cur.rfind(' ');
        std::string rest;
        if (sp != std::string::npos && sp > 0) { rest = cur.substr(sp + 1); cur = cur.substr(0, sp); }
        lines.push_back(cur);
        cur = rest;
        if ((int)lines.size() == max_lines) break;
      }
      cur += ch;
      i += len;
    }
    if ((int)lines.size() < max_lines && !cur.empty()) lines.push_back(cur);
    else if (i < s.size() && !lines.empty()) {
      std::string &l = lines.back();
      while (!l.empty() && fl_width((l + "…").c_str()) > width) l.pop_back();
      l += "…";
    }
    return lines;
  }

  void draw() override {
    draw_wallpaper();
    fl_font(FL_HELVETICA, 12);
    for (int i = 0; i < (int)items.size(); i++) {
      Item &it = items[i];
      bool sel = selected.count(i) > 0;
      if (sel) {
        Fl_Color c = fld::palette().accent;
        fl_color(fl_color_average(c, FL_BLACK, 0.7f));
        fl_rounded_rectf(it.x + 2, it.y, CELL_W - 4, CELL_H - 4, 8);
      }
      if (it.icon) it.icon->draw(it.x + (CELL_W - ICON) / 2, it.y + 4);
      auto lines = wrap_label(it.name, CELL_W - 8, sel ? 4 : 2);
      int ly = it.y + ICON + 8;
      for (auto &l : lines) {
        int lw = (int)fl_width(l.c_str());
        int lx = it.x + (CELL_W - lw) / 2;
        fl_color(fl_rgb_color(0, 0, 0));
        fl_draw(l.c_str(), lx + 1, ly + 13);
        fl_color(FL_WHITE);
        fl_draw(l.c_str(), lx, ly + 12);
        ly += 15;
      }
    }
    if (banding) {
      fl_color(fld::palette().accent);
      int rx = std::min(bx0, bx1), ry = std::min(by0, by1);
      fl_rect(rx, ry, std::abs(bx1 - bx0), std::abs(by1 - by0));
    }
  }

  int item_at(int ex, int ey) const {
    for (int i = 0; i < (int)items.size(); i++)
      if (ex >= items[i].x && ex < items[i].x + CELL_W && ey >= items[i].y && ey < items[i].y + CELL_H - 4)
        return i;
    return -1;
  }

  void open_item(int i) {
    const Item &it = items[i];
    if (it.path == "trash:///") fld::spawn({"fl-files", "trash:///"});
    else if (it.is_dir) fld::spawn({"fl-files", it.path});
    else fld::open_path(it.path);
  }

  std::vector<std::string> selected_paths() const {
    std::vector<std::string> v;
    for (int i : selected)
      if (items[i].path != "trash:///" && items[i].path != fld::home_dir()) v.push_back(items[i].path);
    return v;
  }

  void trash_selected() {
    auto paths = selected_paths();
    if (paths.empty()) return;
    std::string err;
    for (auto &p : paths)
      if (!fld::trash(p, &err)) fl_alert("Could not move to trash:\n%s", err.c_str());
    reload();
  }

  void rename_item(int i) {
    Item &it = items[i];
    if (it.path == "trash:///" || it.path == fld::home_dir()) return;
    const char *n = fl_input("New name:", fld::basename(it.path).c_str());
    if (!n || !*n || strchr(n, '/')) return;
    std::string dst = fld::path_join(dir, n);
    if (fld::file_exists(dst)) { fl_alert("\"%s\" already exists.", n); return; }
    if (rename(it.path.c_str(), dst.c_str()) != 0) fl_alert("Rename failed: %s", strerror(errno));
    reload();
  }

  void item_menu(int i) {
    Item &it = items[i];
    bool special = it.path == "trash:///" || it.path == fld::home_dir();
    std::vector<fld::DesktopEntry> apps;
    if (!special && !it.is_dir) apps = fld::apps_for_mime(fld::mime_type(it.path));
    std::vector<Fl_Menu_Item> m;
    m.push_back({"Open", 0, nullptr, (void *)1});
    if (!apps.empty()) {
      m.push_back({"Open With", 0, nullptr, nullptr, FL_SUBMENU});
      for (size_t k = 0; k < apps.size() && k < 15; k++)
        m.push_back({apps[k].name.c_str(), 0, nullptr, (void *)(100 + k)});
      m.push_back({nullptr});
    }
    if (it.path == "trash:///") m.push_back({"Empty Trash", 0, nullptr, (void *)5});
    if (!special) {
      m.back().flags |= FL_MENU_DIVIDER;
      m.push_back({"Rename…", 0, nullptr, (void *)2});
      m.push_back({"Move to Trash", 0, nullptr, (void *)3});
    }
    m.push_back({nullptr});
    const Fl_Menu_Item *r = m[0].popup(Fl::event_x(), Fl::event_y());
    if (!r) return;
    long id = (long)r->user_data();
    if (id == 1) open_item(i);
    else if (id == 2) rename_item(i);
    else if (id == 3) trash_selected();
    else if (id == 5) {
      if (fl_choice("Permanently delete all items in the Trash?", "Cancel", "Empty Trash", nullptr) == 1) {
        fld::trash_empty();
        reload();
      }
    } else if (id >= 100) apps[id - 100].launch({it.path});
  }

  void new_item(bool folder) {
    const char *n = fl_input(folder ? "Folder name:" : "File name:", folder ? "New Folder" : "New File.txt");
    if (!n || !*n || strchr(n, '/')) return;
    std::string p = fld::unique_name(dir, n);
    if (folder) mkdir(p.c_str(), 0755);
    else fld::write_file(p, "");
    reload();
  }

  void choose_wallpaper() {
    Fl_File_Chooser fc(fld::dirname(wallpaper).c_str(), "Images (*.{png,jpg,jpeg,svg,gif,bmp,webp})",
                       Fl_File_Chooser::SINGLE, "Choose Wallpaper");
    fc.preview(1);
    fc.show();
    while (fc.shown()) Fl::wait();
    if (!fc.value()) return;
    fld::Ini cfg;
    cfg.load(fld::desktop_config_path());
    cfg.set("Desktop", "Wallpaper", fc.value());
    cfg.save(fld::desktop_config_path());
    load_config();
  }

  void set_mode(const char *m) {
    fld::Ini cfg;
    cfg.load(fld::desktop_config_path());
    cfg.set("Desktop", "WallpaperMode", m);
    cfg.save(fld::desktop_config_path());
    load_config();
  }

  void background_menu() {
    Fl_Menu_Item m[] = {
        {"New Folder…", 0, nullptr, (void *)1},
        {"New Empty File…", 0, nullptr, (void *)2, FL_MENU_DIVIDER},
        {"Open Terminal Here", 0, nullptr, (void *)3},
        {"Open Desktop Folder", 0, nullptr, (void *)4, FL_MENU_DIVIDER},
        {"Change Wallpaper…", 0, nullptr, (void *)5},
        {"Wallpaper Style", 0, nullptr, nullptr, FL_SUBMENU},
          {"Fill", 0, nullptr, (void *)10, FL_MENU_RADIO | (mode == "fill" ? FL_MENU_VALUE : 0)},
          {"Fit", 0, nullptr, (void *)11, FL_MENU_RADIO | (mode == "fit" ? FL_MENU_VALUE : 0)},
          {"Stretch", 0, nullptr, (void *)12, FL_MENU_RADIO | (mode == "stretch" ? FL_MENU_VALUE : 0)},
          {"Center", 0, nullptr, (void *)13, FL_MENU_RADIO | (mode == "center" ? FL_MENU_VALUE : 0)},
          {"Tile", 0, nullptr, (void *)14, FL_MENU_RADIO | (mode == "tile" ? FL_MENU_VALUE : 0)},
          {nullptr},
        {"Show Desktop Icons", 0, nullptr, (void *)6, FL_MENU_TOGGLE | (show_icons ? FL_MENU_VALUE : 0),
         0, 0, 0, 0},
        {"Refresh", 0, nullptr, (void *)7},
        {nullptr}};
    const Fl_Menu_Item *r = m->popup(Fl::event_x(), Fl::event_y());
    if (!r) return;
    switch ((long)r->user_data()) {
      case 1: new_item(true); break;
      case 2: new_item(false); break;
      case 3: fld::spawn(fld::shell_split(fld::terminal_command()), dir); break;
      case 4: fld::spawn({"fl-files", dir}); break;
      case 5: choose_wallpaper(); break;
      case 6: {
        fld::Ini cfg;
        cfg.load(fld::desktop_config_path());
        cfg.set_bool("Desktop", "ShowIcons", !show_icons);
        cfg.save(fld::desktop_config_path());
        load_config();
        reload();
        break;
      }
      case 7: load_config(); reload(); break;
      case 10: set_mode("fill"); break;
      case 11: set_mode("fit"); break;
      case 12: set_mode("stretch"); break;
      case 13: set_mode("center"); break;
      case 14: set_mode("tile"); break;
    }
  }

  int handle(int e) override {
    switch (e) {
      case FL_FOCUS: case FL_UNFOCUS: return 1;
      case FL_PUSH: {
        take_focus();
        int i = item_at(Fl::event_x(), Fl::event_y());
        bool ctrl = Fl::event_state() & FL_CTRL;
        if (Fl::event_button() == FL_RIGHT_MOUSE) {
          if (i >= 0) {
            if (!selected.count(i)) { selected.clear(); selected.insert(i); redraw(); }
            item_menu(i);
          } else background_menu();
          return 1;
        }
        if (i >= 0) {
          if (ctrl) { if (selected.count(i)) selected.erase(i); else selected.insert(i); }
          else if (!selected.count(i)) { selected.clear(); selected.insert(i); }
          if (Fl::event_clicks()) { Fl::event_clicks(0); open_item(i); }
        } else {
          if (!ctrl) selected.clear();
          banding = true;
          bx0 = bx1 = Fl::event_x();
          by0 = by1 = Fl::event_y();
        }
        redraw();
        return 1;
      }
      case FL_DRAG:
        if (banding) {
          bx1 = Fl::event_x();
          by1 = Fl::event_y();
          int rx0 = std::min(bx0, bx1), rx1 = std::max(bx0, bx1), ry0 = std::min(by0, by1), ry1 = std::max(by0, by1);
          if (!(Fl::event_state() & FL_CTRL)) selected.clear();
          for (int i = 0; i < (int)items.size(); i++) {
            const Item &it = items[i];
            if (it.x + CELL_W > rx0 && it.x < rx1 && it.y + CELL_H > ry0 && it.y < ry1) selected.insert(i);
          }
          redraw();
        }
        return 1;
      case FL_RELEASE:
        banding = false;
        redraw();
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if (k == FL_Delete) { trash_selected(); return 1; }
        if (k == FL_F + 2 && selected.size() == 1) { rename_item(*selected.begin()); return 1; }
        if ((k == FL_Enter || k == FL_KP_Enter) && !selected.empty()) {
          for (int i : selected) open_item(i);
          return 1;
        }
        if (k == FL_F + 5) { reload(); return 1; }
        if ((Fl::event_state() & FL_CTRL) && k == 'a') {
          for (int i = 0; i < (int)items.size(); i++) selected.insert(i);
          redraw();
          return 1;
        }
        return 0;
      }
    }
    return Fl_Widget::handle(e);
  }
};

static DesktopView *g_view;
static int g_inotify = -1;

static void reload_later(void *) { g_view->reload(); }

static void inotify_cb(int fd, void *) {
  char buf[4096];
  while (read(fd, buf, sizeof buf) > 0) {}
  Fl::remove_timeout(reload_later);
  Fl::add_timeout(0.3, reload_later);
}

static void config_poll(void *) {
  // pick up wallpaper changes made by other programs
  static time_t last = 0;
  struct stat st;
  if (stat(fld::desktop_config_path().c_str(), &st) == 0 && st.st_mtime != last) {
    if (last != 0) { g_view->load_config(); g_view->reload(); }
    last = st.st_mtime;
  }
  Fl::repeat_timeout(2.0, config_poll);
}

int main(int argc, char **argv) {
  fld::init_app("fldesktop.desktop", argc, argv);
  Fl_Double_Window *win = new Fl_Double_Window(1280, 720, "fldesktop.desktop");
  win->border(0);
  g_view = new DesktopView(0, 0, 1280, 720);
  g_view->dir = fld::user_dir("DESKTOP");
  win->resizable(g_view);
  win->end();
  g_view->load_config();
  g_view->reload();

  g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (g_inotify >= 0) {
    inotify_add_watch(g_inotify, g_view->dir.c_str(),
                      IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB);
    inotify_add_watch(g_inotify, (fld::trash_dir() + "/files").c_str(), IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM);
    Fl::add_fd(g_inotify, FL_READ, inotify_cb);
  }
  Fl::add_timeout(2.0, config_poll);
  win->size(Fl::w(), Fl::h());
  Fl::add_handler([](int e) -> int {
    if (e == FL_SCREEN_CONFIGURATION_CHANGED && g_view->window()) g_view->window()->size(Fl::w(), Fl::h());
    return 0;
  });
  win->show(argc, argv);
  return Fl::run();
}
