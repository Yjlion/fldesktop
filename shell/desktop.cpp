// fl-desktop: wallpaper and desktop icons
//
// app_id "fldesktop.desktop": fl-wm keeps it below all windows and sizes it
// to the primary output. Icons are the files in the XDG desktop folder.
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Image.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_RGB_Image.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <memory>
#include <map>
#include <set>
#include <string>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "fld/config_watch.h"
#include "fld/desktop_entry.h"
#include "fld/drag.h"
#include "fld/fileops.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/trash.h"
#include "fld/util.h"
#include "desktop_layout.h"
#include "panel.h" // PANEL_H

struct Item {
  std::string path, name, icon_name;
  bool is_dir = false;
  Fl_Image *icon = nullptr;
  int x = 0, y = 0;
};

// icon sizes for zooming (Ctrl+wheel, Ctrl +/-/0, the Icon Size menu)
static const int kIconSizes[] = {32, 48, 64, 80, 96, 128};
static const int kIconSizeCount = (int)(sizeof kIconSizes / sizeof kIconSizes[0]);
static const int kDefaultIcon = 48;
static int ICON = kDefaultIcon, CELL_W = 100, CELL_H = 96;
static void set_icon_size(int px) {
  ICON = px;
  CELL_W = std::max(100, px + 52);
  CELL_H = px + 48;
}

class DesktopView : public Fl_Widget {
public:
  std::string dir;
  std::vector<Item> items;
  std::set<int> selected;
  std::unique_ptr<Fl_RGB_Image> sel_img_[2];
  std::vector<uchar> sel_buf_[2];
  Fl_Color sel_accent_ = 0;
  std::string wallpaper, mode = "fill";
  Fl_Color bg_color = fl_rgb_color(0x2d, 0x3a, 0x4f);
  bool show_icons = true;
  bool panel_top = false; // fl-panel is at the top edge (wm.ini behaviour/panel_position)
  // only the wallpaper scaled to the screen is kept: the file is decoded
  // again when the screen size or the wallpaper changes (a 4K photo is
  // about 33 MB decoded)
  std::unique_ptr<Fl_Image> wp_scaled;
  bool wp_missing = false;
  int wp_w = 0, wp_h = 0;
  // rubber band
  bool banding = false;
  int bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
  // drag and drop
  int press_item = -1, press_x = 0, press_y = 0, pending_toggle = -1;
  bool dragging = false, dropping = false;
  int drop_item = -1; // folder, Home, Trash or launcher under a drag
  int drop_x = 0, drop_y = 0;
  // our own drag: the icons it moves (Home and Trash too) and the one under
  // the pointer, to move them on the desktop when they are dropped on it
  std::vector<std::string> drag_keys;
  std::set<std::string> drag_paths;
  std::string drag_anchor;
  // Home and Trash alone (no files to hand to other programs), or no
  // Wayland drag: the icons are moved within the desktop on release
  bool moving_here = false;
  // icon cells: the ones the user moved (desktop-icons.ini) and the grid
  std::map<std::string, desk::Cell> saved;
  int grid_cols = 1, grid_rows = 1;


  DesktopView(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}

  void zoom(int delta) {
    int i = 0;
    while (i < kIconSizeCount - 1 && kIconSizes[i] < ICON) i++;
    apply_icon_size(delta == 0 ? kDefaultIcon : kIconSizes[std::max(0, std::min(kIconSizeCount - 1, i + delta))]);
  }

  void apply_icon_size(int px) {
    if (px == ICON) return;
    fld::Ini cfg;
    cfg.load(fld::desktop_config_path());
    cfg.set_int("Desktop", "IconSize", px);
    cfg.save(fld::desktop_config_path());
    set_icon_size(px);
    reload();
  }

  static bool is_launcher(const Item &it) { return !it.is_dir && fld::ends_with(it.path, ".desktop"); }

  // folders, Home and Trash take files; launchers open them
  bool drop_target(int i) const {
    if (i < 0 || i >= (int)items.size()) return false;
    const Item &it = items[i];
    if (it.path == "trash:///" || it.is_dir) return true;
    if (!is_launcher(it)) return false;
    fld::DesktopEntry e;
    return e.load(it.path) && !e.exec.empty();
  }

  // the name an icon's cell is saved under
  std::string key_of(const Item &it) const {
    if (it.path == "trash:///") return ":trash";
    if (it.path == fld::home_dir()) return ":home";
    return fld::basename(it.path);
  }

  static std::string positions_path() { return fld::path_join(fld::fld_config_dir(), "desktop-icons.ini"); }

  void load_positions() {
    saved.clear();
    fld::Ini ini;
    if (!ini.load(positions_path())) return;
    for (auto &k : ini.keys("Positions")) {
      desk::Cell c;
      if (desk::parse_cell(ini.get("Positions", k), c)) saved[fld::uri_decode(k)] = c;
    }
  }

  void save_positions() {
    fld::Ini ini;
    for (auto &kv : saved) ini.set("Positions", fld::uri_encode_path(kv.first), desk::format_cell(kv.second));
    ini.save(positions_path());
  }

  // the grid cell under a point (on the grid)
  desk::Cell cell_at(int ex, int ey) const {
    int top = panel_top ? PANEL_H : 0;
    int col = (ex - x() - 8) / CELL_W, row = (ey - y() - top - 8) / CELL_H;
    return {std::min(std::max(col, 0), grid_cols - 1), std::min(std::max(row, 0), grid_rows - 1)};
  }

  desk::Cell cell_of(const Item &it) const { return cell_at(it.x + CELL_W / 2, it.y + CELL_H / 2); }

  // Moves the icons `moving` so that `anchor` is in the cell under (ex, ey);
  // icons not on the desktop yet (files being dropped) go below it. Every
  // icon keeps the cell it has now, so the others do not shuffle.
  void move_icons(const std::vector<std::string> &moving, const std::string &anchor, int ex, int ey) {
    std::vector<std::string> keys;
    std::vector<desk::Cell> cells;
    for (auto &it : items) {
      keys.push_back(key_of(it));
      cells.push_back(cell_of(it));
    }
    int below = 0;
    desk::Cell at = cell_at(ex, ey);
    for (auto &k : moving)
      if (std::find(keys.begin(), keys.end(), k) == keys.end()) {
        keys.push_back(k);
        // offsets from the anchor, which is the first of them if not here
        cells.push_back({at.first, at.second + below++});
      }
    std::string a = anchor;
    if (std::find(moving.begin(), moving.end(), a) == moving.end() && !moving.empty()) a = moving[0];
    desk::Cell target = at;
    for (size_t i = 0; i < keys.size(); i++)
      if (keys[i] == a && i >= items.size()) target = cells[i];
    for (auto &kv : desk::move(keys, cells, moving, a, target, grid_cols, grid_rows)) saved[kv.first] = kv.second;
    save_positions();
    layout_items();
    redraw();
  }

  // files dropped on icon i: into a folder (Home, Trash), or opened with a launcher
  void drop_on(int i, const std::vector<std::string> &paths, int state) {
    if (i < 0 || i >= (int)items.size()) return;
    const Item &it = items[i];
    if (is_launcher(it)) {
      fld::DesktopEntry e;
      if (e.load(it.path)) e.launch(paths);
      return;
    }
    std::string dest = it.path;
    fld::run_drop(fld::drop_action(paths, dest, state), paths, dest, [](FileOp *) {});
  }

  void arrange_icons() {
    saved.clear();
    unlink(positions_path().c_str());
    layout_items();
    redraw();
  }

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
    {
      fld::Ini wm;
      wm.load(fld::wm_config_path());
      panel_top = fld::lower(wm.get("behaviour", "panel_position", "bottom")) == "top";
    }
    int size = cfg.get_int("Desktop", "IconSize", kDefaultIcon);
    int best = kDefaultIcon;
    for (int s : kIconSizes) if (std::abs(s - size) < std::abs(best - size)) best = s;
    set_icon_size(best);
    wp_scaled.reset();
    wp_missing = false;
    wp_w = wp_h = 0;
    redraw();
  }

  void reload() {
    items.clear();
    selected.clear();
    drop_item = -1; // an index into the old list
    press_item = -1;
    load_positions();
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
    // fl-desktop covers the whole output; keep the icons off the panel's edge
    int top = panel_top ? PANEL_H : 0, bottom = panel_top ? 0 : PANEL_H;
    grid_rows = std::max(1, (h() - top - bottom - 16) / CELL_H);
    grid_cols = std::max(1, (w() - 16) / CELL_W);
    std::vector<std::string> keys;
    for (auto &it : items) keys.push_back(key_of(it));
    auto cells = desk::place(keys, saved, grid_cols, grid_rows);
    for (size_t i = 0; i < items.size(); i++) {
      items[i].x = x() + 8 + cells[i].first * CELL_W;
      items[i].y = y() + top + 8 + cells[i].second * CELL_H;
    }
  }

  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    layout_items();
  }

  void draw_wallpaper() {
    fl_color(bg_color);
    fl_rectf(x(), y(), w(), h());
    if ((!wp_scaled || wp_w != w() || wp_h != h()) && !(wp_missing && wp_w == w() && wp_h == h())) {
      wp_scaled.reset();
      wp_w = w();
      wp_h = h();
      std::unique_ptr<Fl_Image> wp_src(fld::load_image(wallpaper));
      wp_missing = !wp_src;
      if (!wp_src) return;
      int iw = wp_src->w(), ih = wp_src->h();
      double sx = (double)w() / iw, sy = (double)h() / ih;
      double s = mode == "fit" ? std::min(sx, sy) : mode == "center" || mode == "tile" ? 1.0 : std::max(sx, sy);
      if (mode == "stretch") {
        wp_scaled.reset(wp_src->copy(w(), h()));
      } else {
        int nw = std::max(1, (int)std::lround(iw * s)), nh = std::max(1, (int)std::lround(ih * s));
        wp_scaled.reset(wp_src->copy(nw, nh));
      }
      // the copy of an SVG draws it at the new size: make that now, while
      // the source is still here
      if (auto *rgb = dynamic_cast<Fl_RGB_Image *>(wp_scaled.get())) rgb->normalize();
    }
    if (!wp_scaled) return;
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
      bool sel = selected.count(i) > 0 || i == drop_item;
      if (sel) {
        // translucent so the wallpaper shows through: the accent with an
        // outline while the desktop has the focus, a neutral tint otherwise
        bool focused = Fl::focus() == this || i == drop_item;
        if (Fl_RGB_Image *img = selection_image(focused)) img->draw(it.x + 2, it.y);
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

  // A rounded cell-sized highlight with an alpha channel (FLTK has no
  // translucent fills); rebuilt when the cell size or accent changes.
  Fl_RGB_Image *selection_image(bool focused) {
    int w = CELL_W - 4, h = CELL_H - 4;
    Fl_Color accent = fld::palette().accent;
    std::unique_ptr<Fl_RGB_Image> &img = focused ? sel_img_[1] : sel_img_[0];
    std::vector<uchar> &buf = focused ? sel_buf_[1] : sel_buf_[0];
    if (img && img->w() == w && img->h() == h && sel_accent_ == accent) return img.get();
    if (sel_accent_ != accent) { sel_img_[0].reset(); sel_img_[1].reset(); sel_accent_ = accent; }
    uchar ar, ag, ab;
    Fl::get_color(accent, ar, ag, ab);
    uchar fr = focused ? ar : 0xd0, fg = focused ? ag : 0xd4, fb = focused ? ab : 0xda;
    const double fill_a = focused ? 0.38 : 0.26, edge_a = focused ? 0.95 : 0.0, r = 8;
    buf.assign((size_t)w * h * 4, 0);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++) {
        // distance outside the rounded rectangle (<= 0 inside), for antialiased edges
        double cx = std::min(std::max(x + 0.5, r), w - r), cy = std::min(std::max(y + 0.5, r), h - r);
        double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy) - r;
        double cover = std::min(1.0, std::max(0.0, 0.5 - d));
        if (cover <= 0) continue;
        double edge = std::min(1.0, std::max(0.0, 1.0 - std::fabs(d + 0.5)));
        double a = fill_a * (1 - edge) + edge_a * edge;
        if (edge_a == 0) a = fill_a;
        uchar *p = &buf[((size_t)y * w + x) * 4];
        p[0] = fr; p[1] = fg; p[2] = fb;
        p[3] = (uchar)std::lround(255 * a * cover);
      }
    img.reset(new Fl_RGB_Image(buf.data(), w, h, 4));
    return img.get();
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
        {"Icon Size", 0, nullptr, nullptr, FL_SUBMENU},
          {"Small", 0, nullptr, (void *)20, FL_MENU_RADIO | (ICON == 32 ? FL_MENU_VALUE : 0)},
          {"Normal", 0, nullptr, (void *)21, FL_MENU_RADIO | (ICON == 48 ? FL_MENU_VALUE : 0)},
          {"Large", 0, nullptr, (void *)22, FL_MENU_RADIO | (ICON == 64 || ICON == 80 ? FL_MENU_VALUE : 0)},
          {"Extra Large", 0, nullptr, (void *)23, FL_MENU_RADIO | (ICON >= 96 ? FL_MENU_VALUE : 0)},
          {nullptr},
        {"Show Desktop Icons", 0, nullptr, (void *)6, FL_MENU_TOGGLE | (show_icons ? FL_MENU_VALUE : 0),
         0, 0, 0, 0},
        {"Arrange Icons", 0, nullptr, (void *)8, saved.empty() ? FL_MENU_INACTIVE : 0},
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
      case 8: arrange_icons(); break;
      case 10: set_mode("fill"); break;
      case 11: set_mode("fit"); break;
      case 12: set_mode("stretch"); break;
      case 13: set_mode("center"); break;
      case 14: set_mode("tile"); break;
      case 20: case 21: case 22: case 23: {
        static const int px[] = {32, 48, 64, 96};
        apply_icon_size(px[(long)r->user_data() - 20]);
        break;
      }
    }
  }

  int handle(int e) override {
    switch (e) {
      case FL_FOCUS: case FL_UNFOCUS: if (!selected.empty()) redraw(); return 1;
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
        press_item = i;
        press_x = Fl::event_x();
        press_y = Fl::event_y();
        dragging = false;
        moving_here = false;
        pending_toggle = -1;
        if (i >= 0) {
          // Ctrl on a selected icon deselects it on release, unless it is dragged
          if (ctrl) { if (selected.count(i)) pending_toggle = i; else selected.insert(i); }
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
        if (moving_here) return 1;
        if (!banding && press_item >= 0 && press_item < (int)items.size() && !dragging &&
            (Fl::event_state() & FL_BUTTON1) &&
            (std::abs(Fl::event_x() - press_x) > 6 || std::abs(Fl::event_y() - press_y) > 6)) {
          pending_toggle = -1;
          drag_keys.clear();
          for (int k : selected) drag_keys.push_back(key_of(items[k]));
          drag_anchor = key_of(items[press_item]);
          auto paths = selected_paths();
          drag_paths = std::set<std::string>(paths.begin(), paths.end());
          if (paths.empty()) {
            // Home and Trash alone: only moved on the desktop
            moving_here = true;
            return 1;
          }
          dragging = true;
          std::string text = fld::make_uri_list(paths);
          // the icons under the pointer while dragging, with their number
          fld::DragImage img;
          img.size = ICON;
          if (items[press_item].icon) img.icons.push_back(items[press_item].icon);
          for (int k : selected)
            if (k != press_item && k < (int)items.size() && items[k].icon && img.icons.size() < 3)
              img.icons.push_back(items[k].icon);
          img.count = (int)paths.size();
          auto *self = this;
          if (!fld::start_drag(window(), text, true, img, [self](fld::DragEnd) {
                self->dragging = false;
                self->press_item = -1;
                self->redraw();
              })) {
            // no Wayland drag (FLTK's own would never tell us it ended):
            // the icons can still be moved, or dropped on a folder here
            dragging = false;
            moving_here = true;
          }
          return 1;
        }
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
        if (moving_here) {
          moving_here = false;
          int t = item_at(Fl::event_x(), Fl::event_y());
          std::vector<std::string> paths(drag_paths.begin(), drag_paths.end());
          if (drop_target(t) && !selected.count(t) && !paths.empty()) drop_on(t, paths, Fl::event_state());
          else if (t < 0 || selected.count(t)) move_icons(drag_keys, drag_anchor, Fl::event_x(), Fl::event_y());
        }
        banding = false;
        if (!dragging && pending_toggle >= 0 && pending_toggle == press_item) selected.erase(pending_toggle);
        pending_toggle = -1;
        press_item = -1;
        redraw();
        return 1;
      case FL_MOUSEWHEEL:
        if ((Fl::event_state() & FL_CTRL) && Fl::event_dy()) { zoom(Fl::event_dy() < 0 ? 1 : -1); return 1; }
        return 0;
      case FL_DND_ENTER: case FL_DND_DRAG: {
        int i = item_at(Fl::event_x(), Fl::event_y());
        if (!drop_target(i) || (dragging && selected.count(i))) i = -1;
        if (i != drop_item) { drop_item = i; redraw(); }
        return 1;
      }
      case FL_DND_LEAVE:
        if (drop_item != -1) { drop_item = -1; redraw(); }
        return 1;
      case FL_DND_RELEASE:
        dropping = true;
        drop_x = Fl::event_x();
        drop_y = Fl::event_y();
        return 1;
      case FL_PASTE: {
        // files dropped on a folder (Home, Trash) go into it, on a launcher
        // open with it; on the desktop they go into the desktop folder where
        // they were dropped, and the desktop's own icons move there
        if (!dropping) return 1;
        dropping = false;
        int i = drop_item;
        drop_item = -1;
        bool ours = dragging;
        dragging = false;
        redraw();
        auto paths = fld::parse_uri_list(std::string(Fl::event_text(), Fl::event_length()));
        if (paths.empty()) return 1;
        if (!ours && std::set<std::string>(paths.begin(), paths.end()) == drag_paths) ours = true;
        if (i >= 0 && i < (int)items.size()) {
          drop_on(i, paths, Fl::event_state());
          return 1;
        }
        std::vector<std::string> here, elsewhere;
        for (auto &p : paths) (fld::dirname(p) == dir ? here : elsewhere).push_back(p);
        std::vector<std::string> moving;
        if (ours) moving = drag_keys; // with Home and Trash
        else
          for (auto &p : here) moving.push_back(fld::basename(p));
        for (auto &p : elsewhere) moving.push_back(fld::basename(p));
        if (!moving.empty()) move_icons(moving, ours ? drag_anchor : moving[0], drop_x, drop_y);
        if (!elsewhere.empty())
          fld::run_drop(fld::drop_action(elsewhere, dir, Fl::event_state()), elsewhere, dir, [](FileOp *) {});
        return 1;
      }
      case FL_KEYBOARD: {
        int k = Fl::event_key();
        if (k == FL_Delete) { trash_selected(); return 1; }
        if (k == FL_F + 2 && selected.size() == 1) { rename_item(*selected.begin()); return 1; }
        if ((k == FL_Enter || k == FL_KP_Enter) && !selected.empty()) {
          for (int i : selected) open_item(i);
          return 1;
        }
        if (k == FL_F + 5) { reload(); return 1; }
        if (Fl::event_state() & FL_CTRL) {
          if (k == '=' || k == '+' || k == FL_KP + '+') { zoom(1); return 1; }
          if (k == '-' || k == FL_KP + '-') { zoom(-1); return 1; }
          if (k == '0' || k == FL_KP + '0') { zoom(0); return 1; }
        }
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

static int g_argc;
static char **g_argv;
static std::string appearance_signature() {
  fld::Ini d;
  d.load(fld::desktop_config_path());
  std::string sig;
  for (const char *k : {"Theme", "Accent", "Font", "FontSize", "Scheme", "IconsFollowAccent", "PanelTheme"})
    sig += "|" + d.get("Appearance", k);
  return sig + "|" + d.get("General", "IconTheme");
}
static std::string g_appearance;

// wallpaper and other changes made by other programs (watched with inotify:
// no polling)
static void config_changed() {
  // a new theme, font or icon theme: start again with it
  if (appearance_signature() != g_appearance) fld::restart_self(g_argc, g_argv);
  g_view->load_config();
  g_view->reload();
}

int main(int argc, char **argv) {
  fld::init_app("fldesktop.desktop", argc, argv);
  fld::drag_setup_later(); // file drags show their icons
  Fl::lock(); // file operations report back with Fl::awake
  // The shell window is borderless and fl-wm knows it by its title; any other
  // window (dialogs, fl_choice) gets an ordinary app id, or fl-wm would give
  // it the shell role too.
  Fl_Window::default_xclass("fl-desktop");
  g_argc = argc;
  g_argv = argv;
  g_appearance = appearance_signature();
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
  static fld::ConfigWatcher config_watch(fld::desktop_config_path(), config_changed);
  static fld::ConfigWatcher wm_watch(fld::wm_config_path(), config_changed); // panel position
  win->size(Fl::w(), Fl::h());
  Fl::add_handler([](int e) -> int {
    if (e == FL_SCREEN_CONFIGURATION_CHANGED && g_view->window()) g_view->window()->size(Fl::w(), Fl::h());
    return 0;
  });
  win->show(); // not show(argc, argv): it resets the theme colors
  return Fl::run();
}
