// fl-files: what its units share. main.cpp has the model, the globals, the
// file view, the menus, the window and main(); operations.cpp undo, the
// properties and rename dialogs and the context menus; chrome.cpp the places
// and drives; tree.cpp the folder tree; find.cpp searching; tabs.cpp the tabs;
// network.cpp the network locations.
#pragma once
#include "fld/window_move.h"
#ifdef FLD_HAVE_MDNS
#include "mdns.h"
#endif
#include "fld/json.h" // first: X11 headers define Bool
// fl-files: file manager for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Tree.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>
#include "fld/drag.h"
#include "fld/tab_move.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <grp.h>
#include <map>
#include <memory>
#include <pwd.h>
#include <set>
#include <string>
#include <strings.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <csignal>
#include <sys/wait.h>
#include <vector>

#include "fld/fileops.h"
#include "search.h"
#include "preview_util.h"
#include "rename_util.h"
#ifdef FLD_HAVE_UDISKS
#include "udclient.h"
#endif
#ifdef FLD_HAVE_GVFS
#include "gvclient.h"
#include "gvfs_ui.h"
#endif
#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/mime.h"
#include "fld/tab_bar.h"
#include "fld/theme.h"
#include "fld/trash.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fld/text.h"



enum SortKey { SORT_NAME, SORT_SIZE, SORT_TYPE, SORT_DATE, SORT_PERMS, SORT_OWNER, SORT_LOCATION, SORT_COUNT };
enum ViewMode { VIEW_ICONS, VIEW_COMPACT, VIEW_DETAILS };
enum Column { COL_SIZE, COL_TYPE, COL_DATE, COL_PERMS, COL_OWNER, COL_LOCATION, NUM_COLS };
enum PlaceWhere { PLACE_HERE, PLACE_WINDOW, PLACE_TAB };
struct ViewState;
struct Tab;
struct UndoStep;
struct Place;
struct Entry;
struct PaneGuard;
class FileView;
class PlacesBrowser;
class DropTree;

inline const char *TRASH_URI = "trash:///";
// the network neighbourhood (GVfs): servers and shares to connect to
inline const char *NETWORK_URI = "network:///";

// ---- GVfs locations (through gvfs-fuse) ----
inline const char *kColNames[NUM_COLS] = {"Size", "Type", "Modified", "Permissions", "Owner", "Location"};
static const SortKey kColSort[NUM_COLS] = {SORT_SIZE, SORT_TYPE, SORT_DATE, SORT_PERMS, SORT_OWNER, SORT_LOCATION};
inline const char *kDateFormats[] = {"%Y-%m-%d %H:%M", "%d/%m/%Y %H:%M", "%m/%d/%Y %I:%M %p", "%b %e %Y %H:%M", "%c", nullptr};

// Icon sizes (zoom steps); the zoom level is an index into these
static const int kZoomPx[] = {16, 24, 32, 48, 64, 80, 96, 128, 160, 192, 256};
static const int kZoomSmall[] = {16, 16, 16, 20, 24, 24, 24, 32, 32, 32, 48};
static const int kZoomMax = (int)(sizeof kZoomPx / sizeof kZoomPx[0]) - 1;
static const int kZoomDefault = 3; // 48 px
static const int TOOLBAR_H = 42, STATUS_H = 24, MENUBAR_H = 26;

struct ViewState {
  ViewMode view = VIEW_ICONS;
  SortKey sort = SORT_NAME;
  bool desc = false;
  int zoom = kZoomDefault;
};

struct Tab {
  FileView *view = nullptr;
  std::string cwd;
  std::deque<std::string> back, fwd;
  bool searching = false;
  std::unique_ptr<search::Search> search{new search::Search};
  std::string search_desc;
  ViewState v;
  std::string filter;
  bool find_bar = false;
};

struct UndoStep {
  enum Kind { RENAME, CREATE, COPY, MOVE, TRASH } kind;
  std::vector<std::pair<std::string, std::string>> items; // from, to ("" for TRASH; CREATE: "", the new path)
};

struct Place {
  std::string label, path, icon;
  bool bookmark = false;
  std::string volume;      // UDisks2 volume ("" for other places); path is "" while unmounted
  bool can_eject = false;  // shows the eject button
  std::string uri;         // a network location (bookmark or mount); path is "" while not mounted
  std::string gvfs_mount;  // the root URI of a GVfs mount (to unmount it)
  std::string gvfs_volume; // a GVfs volume to mount (a phone, a camera)
};

struct Settings {
  bool show_hidden = false;
  bool thumbnails = true;
  ViewState def;               // defaults for folders without their own settings
  bool per_folder = true;      // remember view and sort per folder
  bool folders_first = true;
  bool single_click = false;
  bool confirm_trash = false;
  int thumb_max_mb = 20;
  int date_fmt = 0;            // index into kDateFormats, or 5 = relative
  bool show_tree = false;      // folder tree instead of places in the sidebar
  bool col_visible[NUM_COLS] = {true, true, true, false, false, true};
  int col_w[NUM_COLS] = {90, 160, 150, 100, 100, 260};
  int sidebar_w = 190;
  bool always_tabs = false;    // the tab bar also with one tab
  bool middle_tab = true;      // middle click opens folders in a new tab (else a window)
  bool menubar = false;        // a menu bar above the toolbar
  bool remote_thumbs = false;  // thumbnails of files on network locations (slow)
};
struct Clip {
  std::vector<std::string> paths;
  bool cut = false;
};
extern ViewState V;   // main.cpp
extern Settings S;
extern Clip g_clip;

// ---- defined in one unit, used in others
extern time_t g_type_ahead_time;
extern std::vector<Place> g_place_list;
extern std::map<Fl_Tree_Item *, std::string> g_tree_paths;
extern Fl_Group *g_sbar;
void fill_places();
void mount_and_go(const std::string &uri, int where = 0);
extern std::vector<UndoStep> g_undo;
extern Fl_Button *g_s_go;
extern Fl_Tile *g_tile;
void search_poll(void *d);
void quick_look(const std::string &path);
const char *undo_label();
void undo();
void connect_to_server();
bool is_remote(const std::string &path);
std::string remote_uri(const std::string &path);
void tree_follow(const std::string &path);
std::string remote_mount_name(const std::string &path);
void toggle_split();
int global_keys(int e);
std::string other_pane_dir();
void record_op(FileOp *o);
void tree_cb(Fl_Widget *, void *);
extern fld::Toolbar *g_toolbar;
extern Fl_Menu_Bar *g_menubar;
void run_op(FileOp::Kind k, const std::vector<std::string> &src, const std::string &dest);
void relayout();
void copy_to_clipboard(bool cut);
void show_tree(bool on);
void paste_into(const std::string &dir);
void select_place_for(const std::string &path);
void show_search_bar(bool on);
void watch_dir(const std::string &dir);
void trash_selected(bool permanent);
std::string bookmark_uri(const std::string &path);
void open_containing(const std::string &path);
void update_nav_buttons();
std::string bookmark_uri(const Place &p);
void activate_tab(int i);
void update_tabbar();
void add_bookmark_uri(const std::string &uri, const std::string &label = "");
std::string settings_path();
std::string folder_settings_path();
std::string location_name(const std::string &cwd, bool searching, const std::string &desc);
void save_settings();
void add_bookmark(const std::string &path);
void build_search_bar(int W, int Y);
void show_location();
void new_tab(const std::string &path, bool activate = true);
void remove_bookmark(const std::string &uri);
void rename_bookmark(const std::string &uri, const std::string &old_label);
void new_item(bool folder);
void close_tab(int i);
void open_place(const Place &p, PlaceWhere where = PLACE_HERE);
void open_dir_elsewhere(const std::string &path);
void leave_folder(const std::string &dir);
void update_view_button();
void set_view(ViewMode m);
void set_sort(SortKey k, bool desc);
ViewState folder_view(const std::string &dir);
void set_zoom(int z);
void zoom(int delta);
void navigate(const std::string &path, bool record = true);
void update_zoom_slider();
void unmount_place(const Place &p, bool eject);
void properties(const std::vector<std::string> &paths_in);
void remember_view(const std::string &dir);
void columns_menu(int x, int y);
fld::TabBar *make_tabbar(int W);
std::string format_date(time_t t);
void show_preferences();
std::string perm_string(mode_t m);
void refresh();
const std::string &owner_name(uid_t uid);
std::string export_tab(int i);
void update_status();
extern Fl_Double_Window *g_win;
extern FileView *g_view;
extern Fl_Input *g_path;
extern Fl_Input *g_filter;
extern Fl_Box *g_status;
extern Fl_Slider *g_zoom_slider;
extern Fl_Hold_Browser *g_places;
extern Fl_Tree *g_tree;
extern bool g_searching;
extern fld::FlatButton *g_back, *g_fwd, *g_up, *g_empty_trash, *g_view_btn;
extern std::string g_cwd;
extern std::deque<std::string> g_hist_back, g_hist_fwd;
bool import_tab(const std::string &state, int at);
extern int g_inotify, g_watch;
extern search::Search *g_search;
extern std::string g_search_desc;
bool g_search_running();
extern std::vector<std::unique_ptr<Tab>> g_tabs;
extern int g_cur;
extern fld::TabBar *g_tabbar;
extern Fl_Group *g_views;
Tab *cur_tab();
void go_back();
bool in_trash();
bool in_network();
extern std::vector<Entry> g_net_entries;
void go_forward();
extern std::vector<Entry> g_mdns_entries;
void inotify_cb(int fd, void *);
extern std::map<std::string, std::string> g_mdns_names;
void go_up();
std::vector<Entry> network_entries();
void toggle_hidden();
extern bool g_net_listing;
extern std::string g_net_error;
bool in_search();
extern bool g_split;
extern Tab *g_pane[2];
extern int g_pane_focus;
Tab *tab_of(const void *view);
int tab_index(const Tab *t);
void toggle_menubar();
void select_all();
void edit_location();
std::string new_tab_dir();
void batch_rename(const std::vector<std::string> &paths);
void open_entry(Entry &e);
void rebuild_menubar();


struct Entry {
  std::string name, path;
  bool is_dir = false, is_link = false, broken = false;
  unsigned long long size = 0;
  time_t mtime = 0;
  mode_t mode = 0;
  uid_t uid = 0;
  gid_t gid = 0;
  std::string mime;           // computed lazily
  Fl_Image *icon_small = nullptr, *icon_big = nullptr;
  Fl_Image *thumb = nullptr;  // image preview (owned)
  bool thumb_tried = false;
  fld::TrashItem trash;       // in the trash view
  std::string target;         // in the network view: the location it opens
  std::string icon_name;      // its own icon (network view)
  const std::string &get_mime() {
    if (mime.empty()) mime = is_dir ? "inode/directory" : fld::mime_type(path);
    return mime;
  }
};

struct PaneGuard {
  Tab *t = nullptr;
  explicit PaneGuard(const void *view) {
    if (!g_split || view == (const void *)g_view) return;
    t = tab_of(view);
    if (t) swap();
  }
  ~PaneGuard() { if (t) swap(); }
  void swap() {
    std::swap(V, t->v);
    std::swap(g_cwd, t->cwd);
    std::swap(g_searching, t->searching);
  }
};

class FileView : public Fl_Group {
public:
  std::vector<Entry> all;     // everything in the directory
  std::vector<int> items;     // visible (filtered + sorted) indexes into all
  std::set<int> sel;          // selected item positions (into items)
  int focus = -1, anchor = -1;
  int top = 0;                // scroll offset in pixels
  Fl_Scrollbar *sb;
  bool banding = false;
  int bx0, by0, bx1, by1;
  int press_item = -1;
  bool dragging_files = false;
  int drop_item = -1;         // folder highlighted as the drop target
  int hover_item = -1;        // item under the pointer
  std::map<Fl_Image *, Fl_Image *> ghosts; // faded icons of the items being dragged
  // while the view is in a background tab (in_search() etc. are the current tab's)
  bool bg_search = false, bg_trash = false;
  std::string bg_filter;
  struct timespec loaded_mtime{}; // of the folder when it was read
  bool dropping = false;      // the next FL_PASTE is a drop
  int press_x = 0, press_y = 0;
  // a press on a selected item changes the selection on release, so the
  // whole selection can be dragged (Ctrl+drag copies it)
  int pending_toggle = -1; // Ctrl+press on a selected item: deselect it on release
  int pending_single = -1; // press on one of several selected items: select only it
  // drag and drop effects
  float ghost_w = 0.4f;    // how much of the icon a dragged item keeps
  int fade_step = 0;       // a cancelled drag: the faded items come back
  std::string pulse_path;  // the folder files were just dropped into flashes
  int pulse_step = 0;
  int spring_item = -1;    // a folder hovered while dragging opens after a moment
  int scroll_dir = 0;      // dragging near the top (-1) or bottom (1) edge scrolls
  int drag_count = 0;      // how many items are being dragged from here
  int resizing_col = -1, resize_x0 = 0, resize_w0 = 0;
  std::string type_ahead;

  static const int HEADER_H = 26, COMPACT_W = 240;

  static std::set<FileView *> &live() {
    static std::set<FileView *> v;
    return v;
  }

  FileView(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    live().insert(this);
    fld::drag_setup_later();
    sb = new Fl_Scrollbar(x + w - 14, y, 14, h);
    sb->type(FL_VERTICAL);
    sb->callback([](Fl_Widget *s, void *d) {
      FileView *v = (FileView *)d;
      v->top = ((Fl_Scrollbar *)s)->value();
      v->redraw();
    }, this);
    end();
    box(FL_FLAT_BOX);
  }
  ~FileView() override {
    live().erase(this);
    Fl::remove_timeout(fade_cb, this);
    Fl::remove_timeout(pulse_cb, this);
    Fl::remove_timeout(spring_cb, this);
    Fl::remove_timeout(autoscroll_cb, this);
    clear_thumbs();
    clear_ghosts();
  }

  void clear_ghosts() {
    for (auto &g : ghosts) delete g.second;
    ghosts.clear();
  }
  // a faded copy of an icon, for the items being dragged
  Fl_Image *ghost(Fl_Image *ic) {
    auto it = ghosts.find(ic);
    if (it != ghosts.end()) return it->second;
    Fl_Image *g = ic->copy();
    if (g) {
      g->color_average(fld::palette().bg2, ghost_w);
      g->scale(ic->w(), ic->h(), 0, 1);
    }
    return ghosts[ic] = g;
  }
  // the drag of files from this view ended (dropped anywhere or cancelled)
  void end_drag() {
    stop_drop_effects();
    if (!dragging_files) return;
    dragging_files = false;
    clear_ghosts();
    redraw();
    update_status();
  }
  // the drag is over (reported by the drag itself): a cancelled one fades
  // the items back in
  void drag_over(fld::DragEnd e) {
    press_item = -1;
    if (!dragging_files) return;
    if (e == fld::DragEnd::Cancelled) {
      dragging_files = false;
      fade_step = 3;
      Fl::remove_timeout(fade_cb, this);
      Fl::add_timeout(0.05, fade_cb, this);
      update_status();
    } else end_drag();
  }
  static void fade_cb(void *d) {
    FileView *v = (FileView *)d;
    v->fade_step--;
    v->ghost_w = v->fade_step > 0 ? 0.4f + 0.6f * (3 - v->fade_step) / 3.0f : 0.4f;
    v->clear_ghosts();
    v->redraw();
    if (v->fade_step > 0) Fl::repeat_timeout(0.05, fade_cb, d);
  }
  // the folder files were dropped into flashes twice
  void pulse(const std::string &path) {
    pulse_path = path;
    pulse_step = 5;
    Fl::remove_timeout(pulse_cb, this);
    Fl::add_timeout(0.07, pulse_cb, this);
    redraw();
  }
  static void pulse_cb(void *d) {
    FileView *v = (FileView *)d;
    if (--v->pulse_step > 0) Fl::repeat_timeout(0.07, pulse_cb, d);
    v->redraw();
  }
  bool pulsing(const Entry &e) const { return pulse_step > 0 && pulse_step % 2 == 1 && e.path == pulse_path; }
  // spring-loaded folders: hovering one with a drag opens it
  void spring(int i) {
    if (i == spring_item) return;
    Fl::remove_timeout(spring_cb, this);
    spring_item = i;
    if (i >= 0) Fl::add_timeout(1.2, spring_cb, this);
  }
  static void spring_cb(void *d) {
    FileView *v = (FileView *)d;
    int i = v->spring_item;
    v->spring_item = -1;
    if (i < 0 || i >= (int)v->items.size() || i != v->drop_item || !v->entry(i).is_dir) return;
    std::string path = v->entry(i).path;
    v->drop_item = -1;
    navigate(path);
  }
  // auto-scroll while a drag is near the top or bottom edge
  void set_scroll_dir(int d) {
    if (d == scroll_dir) return;
    scroll_dir = d;
    Fl::remove_timeout(autoscroll_cb, this);
    if (d) Fl::add_timeout(0.04, autoscroll_cb, this);
  }
  static void autoscroll_cb(void *d) {
    FileView *v = (FileView *)d;
    if (!v->scroll_dir) return;
    int max_top = std::max(0, v->content_height() - v->view_h());
    int nt = std::max(0, std::min(max_top, v->top + v->scroll_dir * std::max(8, v->row_h() / 2)));
    if (nt != v->top) {
      v->top = nt;
      v->update_scrollbar();
      v->redraw();
    }
    Fl::repeat_timeout(0.04, autoscroll_cb, d);
  }
  void stop_drop_effects() {
    spring(-1);
    set_scroll_dir(0);
  }

  void clear_thumbs() {
    for (auto &e : all) { delete e.thumb; e.thumb = nullptr; e.thumb_tried = false; }
  }
  // after a zoom change: icons and thumbnails are size dependent
  void clear_images() {
    clear_thumbs();
    for (auto &e : all) e.icon_big = e.icon_small = nullptr;
  }

  // ---- geometry
  static int big() { return kZoomPx[V.zoom]; }
  static int small() { return kZoomSmall[V.zoom]; }
  static bool details() { return V.view == VIEW_DETAILS; }
  static int row_h() { return small() + 6; }
  static int cell_w() { return V.view == VIEW_ICONS ? std::max(96, big() + 60) : COMPACT_W; }
  static int cell_h() { return V.view == VIEW_ICONS ? big() + 52 : row_h(); }
  int content_x() const { return x(); }
  int content_w() const { return w() - (sb->visible() ? sb->w() : 0); }
  int view_top() const { return y() + (details() ? HEADER_H : 0); }
  int view_h() const { return h() - (details() ? HEADER_H : 0); }
  int columns() const { return details() ? 1 : std::max(1, content_w() / cell_w()); }

  int content_height() const {
    if (details()) return (int)items.size() * row_h();
    int cols = columns();
    return ((int)items.size() + cols - 1) / cols * cell_h() + 8;
  }

  void item_rect(int i, int &ix, int &iy, int &iw, int &ih) const {
    if (details()) {
      ix = content_x();
      iy = view_top() + i * row_h() - top;
      iw = content_w();
      ih = row_h();
    } else {
      int cols = columns();
      int extra = V.view == VIEW_ICONS ? (content_w() - cols * cell_w()) / 2 : 4;
      ix = content_x() + std::max(0, extra) + (i % cols) * cell_w();
      iy = view_top() + 4 + (i / cols) * cell_h() - top;
      iw = cell_w();
      ih = cell_h();
    }
  }

  int item_at(int ex, int ey) const {
    if (ey < view_top()) return -1;
    if (details()) {
      int i = (ey - view_top() + top) / row_h();
      return i >= 0 && i < (int)items.size() ? i : -1;
    }
    for (int i = first_visible(); i < (int)items.size(); i++) {
      int ix, iy, iw, ih;
      item_rect(i, ix, iy, iw, ih);
      if (iy > y() + h()) break;
      bool hit = V.view == VIEW_ICONS ? ex >= ix + 4 && ex < ix + iw - 4 && ey >= iy && ey < iy + ih - 4
                                      : ex >= ix && ex < ix + iw - 6 && ey >= iy && ey < iy + ih;
      if (hit) return i;
    }
    return -1;
  }

  int first_visible() const {
    if (details()) return std::max(0, top / row_h());
    return std::max(0, (top / cell_h()) * columns());
  }

  void update_scrollbar() {
    int ch = content_height(), vh = view_h();
    sb->linesize(row_h() * 2);
    if (ch > vh) {
      sb->show();
      sb->resize(x() + w() - 14, view_top(), 14, vh);
      top = std::max(0, std::min(top, ch - vh));
      sb->value(top, vh, 0, ch);
    } else {
      sb->hide();
      top = 0;
    }
  }

  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    update_scrollbar();
  }

  void ensure_visible(int i) {
    if (i < 0) return;
    int ix, iy, iw, ih;
    item_rect(i, ix, iy, iw, ih);
    int rel = iy + top - view_top();
    if (rel < top) top = rel - 4;
    else if (rel + ih > top + view_h()) top = rel + ih - view_h() + 4;
    top = std::max(0, top);
    update_scrollbar();
    redraw();
  }

  Entry &entry(int i) { return all[items[i]]; }

  std::vector<std::string> selected_paths() {
    std::vector<std::string> v;
    for (int i : sel) v.push_back(entry(i).path);
    return v;
  }

  // ---- columns of the details view
  std::vector<int> visible_columns() const {
    std::vector<int> v;
    for (int c = 0; c < NUM_COLS; c++) {
      if (c == COL_LOCATION) { if (in_search()) v.push_back(c); continue; }
      if (S.col_visible[c]) v.push_back(c);
    }
    return v;
  }
  int name_col_w() const {
    int used = 16;
    for (int c : visible_columns()) used += S.col_w[c];
    return std::max(160, content_w() - used);
  }
  std::string column_title(int c) const {
    if (in_trash() && c == COL_TYPE) return "Original Location";
    if (in_trash() && c == COL_DATE) return "Deleted";
    return kColNames[c];
  }
  std::string cell_text(Entry &e, int c) {
    switch (c) {
      case COL_SIZE: return e.is_dir ? "" : fld::human_size(e.size);
      case COL_TYPE: return in_trash() ? fld::tilde(fld::dirname(e.trash.original_path)) : fld::mime_description(e.get_mime());
      case COL_DATE: return format_date(e.mtime);
      case COL_PERMS: return perm_string(e.mode);
      case COL_OWNER: return owner_name(e.uid);
      case COL_LOCATION: return fld::tilde(fld::dirname(e.path));
    }
    return "";
  }

  void apply_filter_sort() {
    bool cur = this == g_view;
    std::string f = fld::lower(cur ? (g_filter ? g_filter->value() : "") : bg_filter.c_str());
    bool trash = cur ? in_trash() : bg_trash, search = cur ? in_search() : bg_search;
    items.clear();
    for (int i = 0; i < (int)all.size(); i++) {
      const Entry &e = all[i];
      if (!S.show_hidden && !trash && !search && e.name[0] == '.') continue;
      if (!f.empty() && fld::lower(e.name).find(f) == std::string::npos) continue;
      items.push_back(i);
    }
    auto cmp = [this](int a, int b) {
      Entry &x = all[a], &y = all[b];
      if (S.folders_first && x.is_dir != y.is_dir) return x.is_dir;
      int r = 0;
      switch (V.sort) {
        case SORT_SIZE: r = x.size < y.size ? -1 : x.size > y.size ? 1 : 0; break;
        case SORT_DATE: r = x.mtime < y.mtime ? -1 : x.mtime > y.mtime ? 1 : 0; break;
        case SORT_TYPE: r = strcasecmp(fld::extension(x.name).c_str(), fld::extension(y.name).c_str()); break;
        case SORT_PERMS: r = (x.mode & 07777) < (y.mode & 07777) ? -1 : (x.mode & 07777) > (y.mode & 07777) ? 1 : 0; break;
        case SORT_OWNER: r = owner_name(x.uid).compare(owner_name(y.uid)); break;
        case SORT_LOCATION: r = fld::natural_compare(fld::dirname(x.path), fld::dirname(y.path)); break;
        default: break;
      }
      if (r == 0) r = fld::natural_compare(x.name, y.name);
      return V.desc ? r > 0 : r < 0;
    };
    std::stable_sort(items.begin(), items.end(), cmp);
    sel.clear();
    focus = items.empty() ? -1 : 0;
    anchor = focus;
    update_scrollbar();
    redraw();
  }

  // re-sorts (e.g. while search results arrive) keeping the selection
  void resort_keep_selection() {
    std::set<std::string> selected;
    for (int i : sel) selected.insert(entry(i).path);
    std::string focus_path = focus >= 0 && focus < (int)items.size() ? entry(focus).path : "";
    int t = top;
    apply_filter_sort();
    for (int i = 0; i < (int)items.size(); i++) {
      if (selected.count(entry(i).path)) sel.insert(i);
      if (entry(i).path == focus_path) focus = anchor = i;
    }
    top = t;
    update_scrollbar();
  }

  static bool stat_entry(const std::string &path, Entry &e) {
    e.name = fld::basename(path);
    e.path = path;
    struct stat st, lst;
    if (lstat(path.c_str(), &lst) != 0) return false;
    e.is_link = S_ISLNK(lst.st_mode);
    if (stat(path.c_str(), &st) != 0) { st = lst; e.broken = e.is_link; }
    e.is_dir = S_ISDIR(st.st_mode);
    e.size = st.st_size;
    e.mtime = st.st_mtime;
    e.mode = st.st_mode;
    e.uid = st.st_uid;
    e.gid = st.st_gid;
    return true;
  }

  void load(const std::string &dir, const std::string &select_name = "") {
    clear_thumbs();
    all.clear();
    struct stat dst;
    loaded_mtime = stat(dir == TRASH_URI ? (fld::trash_dir() + "/files").c_str() : dir.c_str(), &dst) == 0
                       ? dst.st_mtim : timespec{};
    if (dir == NETWORK_URI) {
      all = network_entries();
    } else if (dir == TRASH_URI) {
      for (auto &t : fld::trash_list()) {
        Entry e;
        e.trash = t;
        e.name = t.name;
        e.path = fld::trash_dir() + "/files/" + t.name;
        struct stat st;
        if (lstat(e.path.c_str(), &st) == 0) {
          e.is_dir = S_ISDIR(st.st_mode);
          e.size = st.st_size;
          e.mode = st.st_mode;
          e.uid = st.st_uid;
        }
        e.mtime = t.deletion_date;
        all.push_back(e);
      }
    } else {
      for (auto &n : fld::list_dir(dir, true)) {
        Entry e;
        if (stat_entry(fld::path_join(dir, n), e)) all.push_back(e);
      }
    }
    top = 0;
    apply_filter_sort();
    if (!select_name.empty()) {
      for (int i = 0; i < (int)items.size(); i++)
        if (entry(i).name == select_name) { sel = {i}; focus = anchor = i; ensure_visible(i); }
    }
  }

  // search results arrive in batches
  void add_paths(const std::vector<std::string> &paths) {
    for (auto &p : paths) {
      Entry e;
      if (stat_entry(p, e)) all.push_back(e);
    }
    resort_keep_selection();
  }

  // re-reads the entries of a search result list (after renames, deletions...)
  void restat_all() {
    std::vector<Entry> keep;
    for (auto &e : all) {
      Entry n;
      if (stat_entry(e.path, n)) keep.push_back(n);
    }
    clear_thumbs();
    all.swap(keep);
    resort_keep_selection();
  }

  Fl_Image *icon_for(Entry &e, bool big_icon) {
    Fl_Image *&slot = big_icon ? e.icon_big : e.icon_small;
    if (slot) return slot;
    int size = big_icon ? big() : small();
    std::vector<std::string> names;
    if (!e.icon_name.empty()) names.push_back(e.icon_name);
    if (e.is_dir) {
      std::string p = e.path;
      if (p == fld::home_dir()) names.push_back("user-home");
      else if (p == fld::user_dir("DESKTOP")) names.push_back("user-desktop");
      else if (p == fld::user_dir("DOCUMENTS")) names.push_back("folder-documents");
      else if (p == fld::user_dir("DOWNLOAD")) names.push_back("folder-download");
      else if (p == fld::user_dir("PICTURES")) names.push_back("folder-pictures");
      else if (p == fld::user_dir("MUSIC")) names.push_back("folder-music");
      else if (p == fld::user_dir("VIDEOS")) names.push_back("folder-videos");
      names.push_back("folder");
    } else if (fld::ends_with(e.name, ".desktop")) {
      fld::DesktopEntry de;
      if (de.load(e.path)) names.push_back(de.icon);
    }
    for (auto &n : fld::mime_icon_names(e.get_mime())) names.push_back(n);
    slot = fld::icon_first(names, size);
    return slot;
  }

  void make_thumb(Entry &e) {
    e.thumb_tried = true;
    if (!S.thumbnails || e.is_dir || e.size > ((unsigned long long)S.thumb_max_mb << 20)) return;
    if (!S.remote_thumbs && is_remote(e.path)) return; // reading every image over the network is slow
    if (!fld::starts_with(e.get_mime(), "image/")) return;
    std::unique_ptr<Fl_Image> img(fld::load_image(e.path));
    if (!img) return;
    int B = big();
    double s = std::min((double)(B * 2) / img->data_w(), (double)(B * 2) / img->data_h());
    s = std::min(s, 1.0);
    int tw = std::max(1, (int)(img->data_w() * s)), th = std::max(1, (int)(img->data_h() * s));
    e.thumb = img->copy(tw, th);
    double ds = std::min((double)B / tw, (double)B / th);
    ds = std::min(ds, 1.0);
    e.thumb->scale(std::max(1, (int)(tw * ds)), std::max(1, (int)(th * ds)), 0, 1);
  }

  static void thumb_idle(void *d) {
    FileView *v = (FileView *)d;
    PaneGuard guard(v);
    // generate one thumbnail per idle call for visible items
    int first = v->first_visible();
    for (int i = first; i < (int)v->items.size(); i++) {
      int ix, iy, iw, ih;
      v->item_rect(i, ix, iy, iw, ih);
      if (iy > v->y() + v->h()) break;
      Entry &e = v->entry(i);
      if (!e.thumb_tried) {
        v->make_thumb(e);
        if (e.thumb) v->redraw();
        return;
      }
    }
    Fl::remove_idle(thumb_idle, d);
  }

  // icon labels: broken at '-', '_', '.' and spaces first, "…" at the end
  std::vector<std::string> wrap_name(const std::string &s, int width, int max_lines) {
    return fld::wrap_label(s, width, max_lines);
  }

  // header column under x: returns the column (-1 = name) and whether x is on its right border
  int header_hit(int ex, bool &border) const {
    border = false;
    int cx = content_x() + 8 + name_col_w();
    if (ex < cx - 4) return -1;
    if (std::abs(ex - cx) <= 4) { border = true; return -1; }
    for (int c : visible_columns()) {
      int right = cx + S.col_w[c];
      if (std::abs(ex - right) <= 4) { border = true; return c; }
      if (ex >= cx && ex < right) return c;
      cx = right;
    }
    return -2;
  }

  void draw_header() {
    const fld::Palette &p = fld::palette();
    int hx = content_x(), hy = y();
    fl_color(p.bg);
    fl_rectf(hx, hy, w(), HEADER_H);
    fl_font(FL_HELVETICA_BOLD, 12);
    int cx = hx + 8;
    auto title = [&](const std::string &t0, SortKey k, int cw) {
      fl_color(p.fg_dim);
      std::string t = t0;
      if (V.sort == k) t += V.desc ? "  ▾" : "  ▴";
      fl_draw(t.c_str(), cx, hy, cw - 8, HEADER_H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      cx += cw;
      fl_color(p.border);
      fl_yxline(cx - 4, hy + 5, hy + HEADER_H - 5);
    };
    title("Name", SORT_NAME, name_col_w());
    for (int c : visible_columns()) title(column_title(c), kColSort[c], S.col_w[c]);
    fl_color(p.border);
    fl_xyline(hx, hy + HEADER_H - 1, hx + w());
  }

  void draw() override {
    PaneGuard guard(this);
    draw_view();
    if (g_split && this == g_view) { // the current pane is marked
      fl_color(fld::palette().accent);
      fl_xyline(x(), y(), x() + w() - 1);
      fl_xyline(x(), y() + 1, x() + w() - 1);
    }
  }
  void draw_view() {
    const fld::Palette &p = fld::palette();
    fl_push_clip(x(), y(), w(), h());
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    if (details()) draw_header();
    fl_push_clip(x(), view_top(), content_w(), view_h());
    if (banding) { // the rubber band's fill goes under the items, its border over them
      fl_color(fl_color_average(p.accent, p.bg2, 0.15f));
      fl_rectf(std::min(bx0, bx1), std::min(by0, by1) - top, std::abs(bx1 - bx0), std::abs(by1 - by0));
    }
    bool focused = Fl::focus() == this;
    bool any_missing_thumb = false;
    if (items.empty()) {
      fl_font(FL_HELVETICA, 14);
      fl_color(p.fg_dim);
      std::string net = g_net_listing ? "Looking for computers and shares…"
                        : !g_net_error.empty() ? "Could not browse the network: " + g_net_error
                                               : "No computers or shares found. Use Connect to Server… to open one.";
      const char *msg = in_search() ? (g_search_running() ? "Searching…" : "No files found")
                        : in_network() ? net.c_str()
                        : in_trash() ? "Trash is empty"
                        : (*g_filter->value() ? "No matching files" : "Folder is empty");
      fl_draw(msg, x(), view_top(), content_w(), view_h(), FL_ALIGN_CENTER);
    }
    std::vector<int> cols = details() ? visible_columns() : std::vector<int>();
    int B = big(), SM = small();
    for (int i = first_visible(); i < (int)items.size(); i++) {
      int ix, iy, iw, ih;
      item_rect(i, ix, iy, iw, ih);
      if (iy > y() + h()) break;
      Entry &e = entry(i);
      bool drop = i == drop_item || pulsing(e);
      bool s = sel.count(i) > 0 && !drop;
      bool hov = i == hover_item && !s && !drop && !banding;
      bool ghosted = (dragging_files || fade_step > 0) && sel.count(i) > 0; // being dragged
      bool cut = g_clip.cut && std::find(g_clip.paths.begin(), g_clip.paths.end(), e.path) != g_clip.paths.end();
      Fl_Color sel_bg = focused ? p.selection : fl_color_average(p.selection, p.bg2, 0.55f);
      Fl_Color hover_bg = fl_color_average(p.selection, p.bg2, 0.12f);
      Fl_Color drop_bg = fl_color_average(p.accent, p.bg2, 0.22f);
      // the drop target: a light accent fill with an accent border
      auto drop_frame = [&](int fx, int fy, int fw, int fh, int r) {
        fl_color(drop_bg);
        fl_rounded_rectf(fx, fy, fw, fh, r);
        fl_color(p.accent);
        fl_line_style(FL_SOLID, 2);
        fl_rounded_rect(fx + 1, fy + 1, fw - 2, fh - 2, r);
        fl_line_style(0);
      };
      auto draw_icon = [&](Fl_Image *ic, int dx, int dy) {
        if (ghosted) ic = ghost(ic);
        if (ic) ic->draw(dx, dy);
      };
      // a folder that takes the drop shows open
      Fl_Image *open_icon = nullptr;
      if (i == drop_item) open_icon = fld::icon_first({"folder-open"}, V.view == VIEW_ICONS ? B : SM);
      if (V.view == VIEW_DETAILS) {
        if (drop) drop_frame(ix, iy, iw, ih, 3);
        else if (s) {
          fl_color(sel_bg);
          fl_rectf(ix, iy, iw, ih);
        } else if (hov) {
          fl_color(hover_bg);
          fl_rectf(ix, iy, iw, ih);
        } else if (i % 2) {
          fl_color(fl_color_average(p.bg2, p.bg, 0.6f));
          fl_rectf(ix, iy, iw, ih);
        }
        Fl_Color fg = s ? p.accent_fg : p.fg;
        if (Fl_Image *ic = open_icon ? open_icon : icon_for(e, false)) draw_icon(ic, ix + 8, iy + (ih - SM) / 2);
        fl_font(FL_HELVETICA, 13);
        fl_color(cut || ghosted ? (s ? fl_color_average(fg, sel_bg, 0.6f) : p.fg_dim) : fg);
        int cx = ix + 8;
        int nw = name_col_w();
        fl_draw(fld::escape_at(fld::ellipsize(e.name, nw - SM - 16)).c_str(), cx + SM + 8, iy, nw - SM - 16, ih,
                FL_ALIGN_LEFT | FL_ALIGN_CLIP);
        cx += nw;
        fl_color(s ? p.accent_fg : p.fg_dim);
        for (int c : cols) {
          int cw = S.col_w[c];
          std::string t = fld::escape_at(cell_text(e, c));
          if (c == COL_PERMS) fl_font(FL_COURIER, 12);
          fl_draw(t.c_str(), cx, iy, cw - (c == COL_SIZE ? 12 : 8), ih,
                  (c == COL_SIZE ? FL_ALIGN_RIGHT : FL_ALIGN_LEFT) | FL_ALIGN_CLIP);
          if (c == COL_PERMS) fl_font(FL_HELVETICA, 13);
          cx += cw;
        }
      } else if (V.view == VIEW_COMPACT) {
        if (drop) drop_frame(ix, iy, iw - 6, ih, 4);
        else if (s || hov) {
          fl_color(s ? sel_bg : hover_bg);
          fl_rounded_rectf(ix, iy, iw - 6, ih, 4);
        }
        if (Fl_Image *ic = open_icon ? open_icon : icon_for(e, false)) draw_icon(ic, ix + 4, iy + (ih - SM) / 2);
        fl_font(FL_HELVETICA, 13);
        fl_color(s ? (ghosted ? fl_color_average(p.accent_fg, sel_bg, 0.6f) : p.accent_fg) : cut || ghosted ? p.fg_dim : p.fg);
        fl_draw(fld::escape_at(fld::ellipsize(e.name, iw - SM - 20)).c_str(), ix + SM + 10, iy, iw - SM - 20, ih,
                FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      } else {
        // icons: the cell gets a light tint and an accent border, the name
        // a solid selection pill
        if (drop) drop_frame(ix + 4, iy, iw - 8, ih - 4, 8);
        else if (s) {
          fl_color(focused ? fl_color_average(p.selection, p.bg2, 0.28f) : fl_color_average(p.selection, p.bg2, 0.16f));
          fl_rounded_rectf(ix + 4, iy, iw - 8, ih - 4, 8);
          fl_color(focused ? p.selection : fl_color_average(p.selection, p.bg2, 0.5f));
          fl_rounded_rect(ix + 4, iy, iw - 8, ih - 4, 8);
        } else if (hov) {
          fl_color(hover_bg);
          fl_rounded_rectf(ix + 4, iy, iw - 8, ih - 4, 8);
        }
        Fl_Image *ic = open_icon ? open_icon : e.thumb ? e.thumb : icon_for(e, true);
        if (!e.thumb_tried && S.thumbnails && !e.is_dir) any_missing_thumb = true;
        if (ic) {
          int icx = ix + (iw - ic->w()) / 2, icy = iy + 4 + (B - ic->h()) / 2;
          draw_icon(ic, icx, icy);
          if (e.thumb) {
            fl_color(p.border);
            fl_rect(icx - 1, icy - 1, ic->w() + 2, ic->h() + 2);
          }
        }
        if (e.is_link) {
          if (Fl_Image *em = fld::icon("emblem-symbolic-link", 16)) em->draw(ix + iw / 2 + B / 4, iy + B - 12);
        }
        fl_font(FL_HELVETICA, 12);
        auto lines = wrap_name(e.name, iw - 16, s ? 3 : 2);
        int ly = iy + B + 8;
        if (s && !lines.empty()) {
          int pw = 0;
          for (auto &l : lines) pw = std::max(pw, (int)fl_width(l.c_str()));
          fl_color(sel_bg);
          fl_rounded_rectf(ix + (iw - pw) / 2 - 4, ly - 1, pw + 8, (int)lines.size() * 15 + 4, 4);
        }
        fl_color(s ? (ghosted ? fl_color_average(p.accent_fg, sel_bg, 0.6f) : p.accent_fg)
                   : cut || ghosted ? p.fg_dim : p.fg);
        for (auto &l : lines) {
          int lw = (int)fl_width(l.c_str());
          fl_draw(l.c_str(), (int)l.size(), ix + (iw - lw) / 2, ly + 12);
          ly += 15;
        }
      }
      if (i == focus && focused && sel.size() > 1 && !s) {
        fl_color(p.accent);
        fl_line_style(FL_DOT);
        fl_rect(ix + 2, iy + 1, iw - 4, ih - 2);
        fl_line_style(0);
      }
    }
    if (banding) {
      int rx = std::min(bx0, bx1), ry = std::min(by0, by1) - top, rw = std::abs(bx1 - bx0), rh = std::abs(by1 - by0);
      fl_color(p.accent);
      fl_rect(rx, ry, rw, rh);
    }
    fl_pop_clip();
    if (sb->visible()) draw_child(*sb);
    fl_pop_clip();
    if (any_missing_thumb && !Fl::has_idle(thumb_idle, this)) Fl::add_idle(thumb_idle, this);
  }

  void select_only(int i) {
    sel.clear();
    if (i >= 0) sel.insert(i);
    focus = anchor = i;
    ensure_visible(i);
    update_status();
  }

  void move_focus(int delta, bool extend) {
    if (items.empty()) return;
    int n = (int)items.size();
    int f = focus < 0 ? 0 : std::max(0, std::min(n - 1, focus + delta));
    if (extend) {
      sel.clear();
      int a = std::min(anchor < 0 ? f : anchor, f), b = std::max(anchor < 0 ? f : anchor, f);
      for (int i = a; i <= b; i++) sel.insert(i);
      focus = f;
      ensure_visible(f);
      update_status();
    } else select_only(f);
  }

  void context_menu(int i);
  void background_menu();
  void start_rename(int i);

  int handle(int e) override;
  int handle_view(int e);
};

class PlacesBrowser : public Fl_Hold_Browser {
public:
  using Fl_Hold_Browser::Fl_Hold_Browser;
  int handle(int e) override {
    if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      void *it = find_item(Fl::event_y());
      int line = it ? lineno(it) : 0;
      if (line <= 0 || line > (int)g_place_list.size()) return 1;
      Place p = g_place_list[(size_t)line - 1];
      bool vol = !p.volume.empty(), mounted = !p.path.empty();
      std::vector<Fl_Menu_Item> m = {{"Open", 0, nullptr, (void *)1},
                                     {"Open in New Tab", 0, nullptr, (void *)8},
                                     {"Open in New Window", 0, nullptr, (void *)2, FL_MENU_DIVIDER}};
      if (!p.gvfs_mount.empty()) { // a network location or a phone
        m.push_back({"Add Bookmark", 0, nullptr, (void *)10});
        m.push_back({"Disconnect", 0, nullptr, (void *)6, FL_MENU_DIVIDER});
      } else if (!p.gvfs_volume.empty()) {
        m.push_back({"Mount", 0, nullptr, (void *)5, FL_MENU_DIVIDER});
      } else if (vol) {
        if (!mounted) m.push_back({"Mount", 0, nullptr, (void *)5});
        else m.push_back({"Unmount", 0, nullptr, (void *)6});
        m.push_back({"Eject", 0, nullptr, (void *)7, (p.can_eject ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER});
      } else {
        m.push_back({"Rename Bookmark…", 0, nullptr, (void *)3, p.bookmark ? 0 : FL_MENU_INACTIVE});
        m.push_back({"Remove Bookmark", 0, nullptr, (void *)4, p.bookmark ? 0 : FL_MENU_INACTIVE});
      }
#ifdef FLD_HAVE_GVFS
      m.back().flags |= FL_MENU_DIVIDER;
      m.push_back({"Connect to Server…", 0, nullptr, (void *)9});
#endif
      m.push_back({nullptr});
      const Fl_Menu_Item *r = m[0].popup(Fl::event_x(), Fl::event_y());
      if (!r) return 1;
      switch ((long)r->user_data()) {
        case 1: case 5: open_place(p); break;
        case 2: open_place(p, PLACE_WINDOW); break;
        case 8: open_place(p, PLACE_TAB); break;
        case 3: rename_bookmark(bookmark_uri(p), p.label); break;
        case 4: remove_bookmark(bookmark_uri(p)); break;
        case 6: unmount_place(p, false); break;
        case 7: unmount_place(p, true); break;
        case 9: connect_to_server(); break;
        case 10: add_bookmark_uri(p.gvfs_mount, p.label); break;
      }
      return 1;
    }
    if (e == FL_PUSH && Fl::event_button() == FL_MIDDLE_MOUSE) {
      int line = line_at(Fl::event_y());
      if (line > 0) open_place(g_place_list[(size_t)line - 1], S.middle_tab ? PLACE_TAB : PLACE_WINDOW);
      return 1;
    }
    // the eject button at the right of a drive
    if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE && Fl::event_x() >= x() + w() - EJECT_W - 4) {
      int line = line_at(Fl::event_y());
      if (line > 0 && g_place_list[(size_t)line - 1].can_eject) {
        unmount_place(g_place_list[(size_t)line - 1], true);
        return 1;
      }
    }
    switch (e) {
      // drops: onto a place (folder or Trash), or a folder below the list to bookmark it
      case FL_DND_ENTER:
        saved_line = value();
        drop_line = -1;
        [[fallthrough]];
      case FL_DND_DRAG: {
        int line = line_at(Fl::event_y());
        if (line > 0 && !drop_target(line)) line = 0;
        if (line != drop_line) {
          drop_line = line;
          value(line > 0 ? line : saved_line);
          // spring-loaded: hovering a place opens it in the view
          Fl::remove_timeout(spring_cb, this);
          if (line > 0 && g_place_list[(size_t)line - 1].path != TRASH_URI) Fl::add_timeout(1.2, spring_cb, this);
        }
        return 1;
      }
      case FL_DND_LEAVE:
        Fl::remove_timeout(spring_cb, this);
        value(saved_line);
        return 1;
      case FL_DND_RELEASE:
        Fl::remove_timeout(spring_cb, this);
        dropping = true;
        return 1;
      case FL_PASTE: {
        if (!dropping) return 1;
        dropping = false;
        int line = line_at(Fl::event_y());
        value(saved_line);
        g_view->end_drag();
        auto paths = fld::parse_uri_list(std::string(Fl::event_text(), Fl::event_length()));
        if (paths.empty()) return 1;
        if (line <= 0) {
          for (auto &p : paths) if (fld::is_dir(p)) add_bookmark(p);
          return 1;
        }
        if (!drop_target(line)) return 1;
        const std::string dest = g_place_list[(size_t)line - 1].path;
        fld::run_drop(fld::drop_action(paths, dest, Fl::event_state()), paths, dest,
                      [](FileOp *o) { record_op(o); refresh(); });
        return 1;
      }
    }
    return Fl_Hold_Browser::handle(e);
  }

protected:
  static const int EJECT_W = 20;
  void item_draw(void *item, int X, int Y, int W, int H) const override {
    int line = lineno(item);
    bool ej = line > 0 && line <= (int)g_place_list.size() && g_place_list[(size_t)line - 1].can_eject;
    Fl_Hold_Browser::item_draw(item, X, Y, ej ? W - EJECT_W : W, H);
    if (ej)
      if (Fl_Image *ic = fld::icon("media-eject", 16)) ic->draw(X + W - EJECT_W + (EJECT_W - 16) / 2, Y + (H - 16) / 2);
  }

private:
  int saved_line = 0, drop_line = -1;
  bool dropping = false;
  int line_at(int y) {
    void *it = find_item(y);
    int line = it ? lineno(it) : 0;
    return line > 0 && line <= (int)g_place_list.size() ? line : 0;
  }
  static bool drop_target(int line) {
    const std::string &p = g_place_list[(size_t)line - 1].path;
    return p == TRASH_URI || (!p.empty() && fld::is_dir(p) && access(p.c_str(), W_OK) == 0);
  }
  ~PlacesBrowser() override { Fl::remove_timeout(spring_cb, this); }
  static void spring_cb(void *d) {
    PlacesBrowser *b = (PlacesBrowser *)d;
    int line = b->drop_line;
    if (line <= 0 || line > (int)g_place_list.size()) return;
    std::string path = g_place_list[(size_t)line - 1].path;
    if (path != g_cwd) navigate(path);
    b->saved_line = b->value();
  }
};

class DropTree : public Fl_Tree {
public:
  using Fl_Tree::Fl_Tree;
  int handle(int e) override {
    switch (e) {
      case FL_DND_ENTER: case FL_DND_DRAG: {
        Fl_Tree_Item *it = item_at();
        if (it != hover) {
          if (hover && hover != first_selected_item()) hover->select(0);
          if (it && !it->is_selected()) it->select(1); else if (it) it = nullptr;
          hover = it;
          redraw();
        }
        return 1;
      }
      case FL_DND_LEAVE: clear_hover(); return 1;
      case FL_DND_RELEASE: dropping = true; return 1;
      case FL_PASTE: {
        if (!dropping) return 1;
        dropping = false;
        clear_hover();
        Fl_Tree_Item *it = item_at();
        g_view->end_drag();
        auto f = it ? g_tree_paths.find(it) : g_tree_paths.end();
        if (f == g_tree_paths.end()) return 1;
        auto paths = fld::parse_uri_list(std::string(Fl::event_text(), Fl::event_length()));
        const std::string dest = f->second;
        fld::run_drop(fld::drop_action(paths, dest, Fl::event_state()), paths, dest,
                      [](FileOp *o) { record_op(o); refresh(); });
        return 1;
      }
    }
    return Fl_Tree::handle(e);
  }

private:
  Fl_Tree_Item *hover = nullptr; // selected only to show the drop target
  bool dropping = false;
  Fl_Tree_Item *item_at() {
    const Fl_Tree_Item *it = find_clicked(1);
    return const_cast<Fl_Tree_Item *>(it);
  }
  void clear_hover() {
    if (hover) { hover->select(0); hover = nullptr; redraw(); }
  }
};
