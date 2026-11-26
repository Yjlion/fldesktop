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


// ---- quick look (Space) ------------------------------------------------------

static time_t g_type_ahead_time = 0;
static pid_t g_preview_pid = -1;
static std::string g_preview_path;

static void reap_preview(void *) {
  if (g_preview_pid <= 0) return;
  if (waitpid(g_preview_pid, nullptr, WNOHANG) != 0) g_preview_pid = -1; // ended (or not our child)
  else Fl::repeat_timeout(1.0, reap_preview);
}

// Space shows the file in its viewer; Space on the same file again closes it
static void quick_look(const std::string &path) {
  if (g_preview_pid > 0 && kill(g_preview_pid, 0) == 0) {
    kill(g_preview_pid, SIGTERM);
    g_preview_pid = -1;
    if (path == g_preview_path) return;
  }
  if (fld::is_dir(path)) return;
  auto cmd = files::preview_command(path, fld::mime_type(path));
  if (cmd.empty() || fld::which(cmd[0]).empty()) return; // nothing previews it
  pid_t pid = fork();
  if (pid == 0) {
    setsid();
    std::vector<char *> argv;
    for (auto &a : cmd) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    execvp(argv[0], argv.data());
    _exit(127);
  }
  if (pid > 0) {
    g_preview_pid = pid;
    g_preview_path = path;
    // reap it when it ends
    Fl::add_timeout(1.0, reap_preview);
  }
}

static const char *TRASH_URI = "trash:///";
// the network neighbourhood (GVfs): servers and shares to connect to
static const char *NETWORK_URI = "network:///";

// ---- GVfs locations (through gvfs-fuse) ----
// a folder of a GVfs mount (a network share, a phone)
static bool is_remote(const std::string &path) {
#ifdef FLD_HAVE_GVFS
  return gv::is_fuse_path(path) || gv::Client::get().state().mount_for_path(path);
#else
  (void)path;
  return false;
#endif
}
// the location a folder of a GVfs mount stands for ("" for other folders)
static std::string remote_uri(const std::string &path) {
#ifdef FLD_HAVE_GVFS
  return gv::uri_for_path(gv::Client::get().state(), path);
#else
  (void)path;
  return "";
#endif
}
// the name of the GVfs mount whose root path is ("" for other folders)
static std::string remote_mount_name(const std::string &path) {
#ifdef FLD_HAVE_GVFS
  const gv::Mount *m = gv::Client::get().state().mount_for_path(path);
  if (m && m->path == path) return m->name;
#else
  (void)path;
#endif
  return "";
}

// ---------------------------------------------------------------------------
// Model

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

enum SortKey { SORT_NAME, SORT_SIZE, SORT_TYPE, SORT_DATE, SORT_PERMS, SORT_OWNER, SORT_LOCATION, SORT_COUNT };
enum ViewMode { VIEW_ICONS, VIEW_COMPACT, VIEW_DETAILS };
// Details view columns besides Name
enum Column { COL_SIZE, COL_TYPE, COL_DATE, COL_PERMS, COL_OWNER, COL_LOCATION, NUM_COLS };
static const char *kColNames[NUM_COLS] = {"Size", "Type", "Modified", "Permissions", "Owner", "Location"};
static const SortKey kColSort[NUM_COLS] = {SORT_SIZE, SORT_TYPE, SORT_DATE, SORT_PERMS, SORT_OWNER, SORT_LOCATION};
static const char *kDateFormats[] = {"%Y-%m-%d %H:%M", "%d/%m/%Y %H:%M", "%m/%d/%Y %I:%M %p", "%b %e %Y %H:%M", "%c", nullptr};

// Icon sizes (zoom steps); the zoom level is an index into these
static const int kZoomPx[] = {16, 24, 32, 48, 64, 80, 96, 128, 160, 192, 256};
static const int kZoomSmall[] = {16, 16, 16, 20, 24, 24, 24, 32, 32, 32, 48};
static const int kZoomMax = (int)(sizeof kZoomPx / sizeof kZoomPx[0]) - 1;
static const int kZoomDefault = 3; // 48 px
static int zoom_for_px(int px) {
  int best = kZoomDefault;
  for (int i = 0; i <= kZoomMax; i++)
    if (std::abs(kZoomPx[i] - px) < std::abs(kZoomPx[best] - px)) best = i;
  return best;
}
// IconSize= (pixels); Zoom= (0-6 for 24-128 px) is the setting of older versions
static int read_zoom(const fld::Ini &c, const std::string &group, int def) {
  static const int old_px[] = {24, 32, 48, 64, 80, 96, 128};
  if (c.has(group, "IconSize")) return zoom_for_px(c.get_int(group, "IconSize", kZoomPx[def]));
  if (c.has(group, "Zoom")) return zoom_for_px(old_px[std::max(0, std::min(6, c.get_int(group, "Zoom", 2)))]);
  return def;
}

// How a folder is shown: per folder when remembered, else the defaults
struct ViewState {
  ViewMode view = VIEW_ICONS;
  SortKey sort = SORT_NAME;
  bool desc = false;
  int zoom = kZoomDefault;
} V;

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
} S;

static std::string settings_path() { return fld::fld_config_dir() + "/files.ini"; }
static std::string folder_settings_path() { return fld::fld_config_dir() + "/files-folders.ini"; }

static void save_settings() {
  fld::Ini c;
  c.load(settings_path());
  c.set_bool("Files", "ShowHidden", S.show_hidden);
  c.set_int("Files", "View", S.def.view);
  c.remove("Files", "ListView");
  c.remove("Files", "Zoom");
  c.set_int("Files", "IconSize", kZoomPx[S.def.zoom]);
  c.set_bool("Files", "Thumbnails", S.thumbnails);
  c.set_int("Files", "Sort", S.def.sort);
  c.set_bool("Files", "SortDescending", S.def.desc);
  c.set_bool("Files", "RememberPerFolder", S.per_folder);
  c.set_bool("Files", "FoldersFirst", S.folders_first);
  c.set_bool("Files", "SingleClick", S.single_click);
  c.set_bool("Files", "ConfirmTrash", S.confirm_trash);
  c.set_int("Files", "ThumbnailMaxMiB", S.thumb_max_mb);
  c.set_int("Files", "DateFormat", S.date_fmt);
  c.set_bool("Files", "FolderTree", S.show_tree);
  c.set_bool("Files", "AlwaysShowTabs", S.always_tabs);
  c.set_bool("Files", "MiddleClickNewTab", S.middle_tab);
  c.set_bool("Files", "ShowMenuBar", S.menubar);
  c.set_bool("Files", "RemoteThumbnails", S.remote_thumbs);
  std::string vis, wid;
  for (int i = 0; i < NUM_COLS; i++) {
    vis += (i ? "," : "") + std::string(S.col_visible[i] ? "1" : "0");
    wid += (i ? "," : "") + std::to_string(S.col_w[i]);
  }
  c.set("Files", "Columns", vis);
  c.set("Files", "ColumnWidths", wid);
  c.save(settings_path());
}

static void load_settings() {
  fld::Ini c;
  c.load(settings_path());
  S.show_hidden = c.get_bool("Files", "ShowHidden", false);
  // ListView= is the setting of older versions
  int view = c.get_int("Files", "View", c.get_bool("Files", "ListView", false) ? VIEW_DETAILS : VIEW_ICONS);
  S.def.view = (ViewMode)std::max(0, std::min(2, view));
  S.def.zoom = read_zoom(c, "Files", kZoomDefault);
  S.thumbnails = c.get_bool("Files", "Thumbnails", true);
  S.def.sort = (SortKey)std::max(0, std::min(SORT_COUNT - 1, c.get_int("Files", "Sort", 0)));
  S.def.desc = c.get_bool("Files", "SortDescending", false);
  S.per_folder = c.get_bool("Files", "RememberPerFolder", true);
  S.folders_first = c.get_bool("Files", "FoldersFirst", true);
  S.single_click = c.get_bool("Files", "SingleClick", false);
  S.confirm_trash = c.get_bool("Files", "ConfirmTrash", false);
  S.thumb_max_mb = std::max(1, std::min(1024, c.get_int("Files", "ThumbnailMaxMiB", 20)));
  S.date_fmt = std::max(0, std::min(5, c.get_int("Files", "DateFormat", 0)));
  S.show_tree = c.get_bool("Files", "FolderTree", false);
  S.always_tabs = c.get_bool("Files", "AlwaysShowTabs", false);
  S.middle_tab = c.get_bool("Files", "MiddleClickNewTab", true);
  S.menubar = c.get_bool("Files", "ShowMenuBar", false);
  S.remote_thumbs = c.get_bool("Files", "RemoteThumbnails", false);
  auto vis = fld::split(c.get("Files", "Columns", ""), ',');
  auto wid = fld::split(c.get("Files", "ColumnWidths", ""), ',');
  for (int i = 0; i < NUM_COLS; i++) {
    if (i < (int)vis.size()) S.col_visible[i] = vis[i] == "1";
    if (i < (int)wid.size()) S.col_w[i] = std::max(40, std::min(1000, atoi(wid[i].c_str())));
  }
  V = S.def;
}

// The view of `dir`: its remembered settings, or the defaults
static ViewState folder_view(const std::string &dir) {
  ViewState v = S.def;
  if (!S.per_folder || dir.empty()) return v;
  fld::Ini c;
  c.load(folder_settings_path());
  if (!c.has(dir, "View")) return v;
  v.view = (ViewMode)std::max(0, std::min(2, c.get_int(dir, "View", v.view)));
  v.sort = (SortKey)std::max(0, std::min(SORT_COUNT - 1, c.get_int(dir, "Sort", v.sort)));
  v.desc = c.get_bool(dir, "SortDescending", v.desc);
  v.zoom = read_zoom(c, dir, v.zoom);
  return v;
}

// Stores a view change for `dir` (or as the default)
static bool in_search();

static void remember_view(const std::string &dir) {
  if (in_search()) return; // search results: just for now
  if (!S.per_folder || dir.empty() || dir.find("://") != std::string::npos) {
    S.def = V;
    save_settings();
    return;
  }
  fld::Ini c;
  c.load(folder_settings_path());
  // keep the file small: the oldest folders go first
  auto groups = c.groups();
  if (groups.size() > 500) c.remove_group(groups.front());
  c.remove_group(dir);
  c.set_int(dir, "View", V.view);
  c.set_int(dir, "Sort", V.sort);
  c.set_bool(dir, "SortDescending", V.desc);
  c.set_int(dir, "IconSize", kZoomPx[V.zoom]);
  c.save(folder_settings_path());
}

static std::string format_date(time_t t) {
  if (S.date_fmt < 5) return fld::format_time(t, kDateFormats[S.date_fmt]);
  // relative
  time_t now = time(nullptr);
  struct tm a, b;
  localtime_r(&t, &a);
  localtime_r(&now, &b);
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) return "Today " + fld::format_time(t, "%H:%M");
  if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday - 1) return "Yesterday " + fld::format_time(t, "%H:%M");
  if (now - t < 6 * 86400 && t <= now) return fld::format_time(t, "%A %H:%M");
  if (a.tm_year == b.tm_year) return fld::format_time(t, "%b %e %H:%M");
  return fld::format_time(t, "%b %e %Y");
}

static std::string perm_string(mode_t m) {
  std::string s = S_ISDIR(m) ? "d" : S_ISLNK(m) ? "l" : "-";
  const char *rwx = "rwxrwxrwx";
  for (int i = 0; i < 9; i++) s += (m & (1 << (8 - i))) ? rwx[i] : '-';
  if (m & S_ISUID) s[3] = (m & S_IXUSR) ? 's' : 'S';
  if (m & S_ISGID) s[6] = (m & S_IXGRP) ? 's' : 'S';
  if (m & S_ISVTX) s[9] = (m & S_IXOTH) ? 't' : 'T';
  return s;
}

static const std::string &owner_name(uid_t uid) {
  static std::map<uid_t, std::string> cache;
  auto it = cache.find(uid);
  if (it != cache.end()) return it->second;
  struct passwd *pw = getpwuid(uid);
  return cache[uid] = pw ? pw->pw_name : std::to_string(uid);
}

// ---------------------------------------------------------------------------
// Globals

class FileView;
static Fl_Double_Window *g_win;
static FileView *g_view;
static Fl_Input *g_path;
static Fl_Input *g_filter;
static Fl_Box *g_status;
static Fl_Slider *g_zoom_slider;
class PlacesBrowser;
static Fl_Hold_Browser *g_places;
class Fl_Tree;
static Fl_Tree *g_tree;
static bool g_searching = false; // the view shows search results
static fld::FlatButton *g_back, *g_fwd, *g_up, *g_empty_trash, *g_view_btn;
static std::string g_cwd;
static std::deque<std::string> g_hist_back, g_hist_fwd;
static int g_inotify = -1, g_watch = -1;

struct Clip {
  std::vector<std::string> paths;
  bool cut = false;
} g_clip;

static search::Search *g_search;  // the current tab's
static std::string g_search_desc; // e.g. "*.txt" for the title and status
static bool g_search_running() { return g_search->running(); }

// Tabs: each has its own view, folder, history, view settings and search.
// The globals above (g_view, g_cwd, g_hist_*, g_searching, g_search*, V) are
// the current tab's; activate_tab() stores them and loads another tab's.
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
static std::vector<std::unique_ptr<Tab>> g_tabs;
static int g_cur = -1;
static fld::TabBar *g_tabbar;
static Fl_Group *g_views; // holds the tabs' views (one shown)
static Tab *cur_tab() { return g_cur >= 0 && g_cur < (int)g_tabs.size() ? g_tabs[(size_t)g_cur].get() : nullptr; }
static void new_tab(const std::string &path, bool activate = true);
static void close_tab(int i);
static std::string export_tab(int i);
static bool import_tab(const std::string &state, int at);
static void activate_tab(int i);
static void update_tabbar();
static void open_dir_elsewhere(const std::string &path); // middle click: a new tab or window
static void leave_folder(const std::string &dir); // tabs in dir (or below) go home
static std::string other_pane_dir();
static void toggle_split();
static void batch_rename(const std::vector<std::string> &paths);

static void navigate(const std::string &path, bool record = true);
static void fill_places();
static void select_place_for(const std::string &path);
static void refresh();
static void update_status();
static void update_zoom_slider();
static void open_entry(Entry &e);
static void run_op(FileOp::Kind k, const std::vector<std::string> &src, const std::string &dest);

static bool in_trash() { return g_cwd == TRASH_URI && !g_searching; }
static bool in_network() { return g_cwd == NETWORK_URI && !g_searching; }
// the network view: what it lists, and why it may be empty
static std::vector<Entry> g_net_entries;
// shares that announce themselves (mDNS): also without gvfsd-dnssd
static std::vector<Entry> g_mdns_entries;
static std::map<std::string, std::string> g_mdns_names; // target -> the server's own name

// what the Network view lists: GVfs' neighbourhood, then the mDNS shares it lacks
static std::vector<Entry> network_entries() {
  std::vector<Entry> v = g_net_entries;
  for (auto &m : g_mdns_entries) {
    bool dup = false;
    for (auto &e : g_net_entries) dup |= e.target == m.target || e.name == m.name || e.name == g_mdns_names[m.target];
    if (!dup) v.push_back(m);
  }
  return v;
}
static bool g_net_listing = false;
static std::string g_net_error;
static void mount_and_go(const std::string &uri, int where = 0); // where: 0 here, 1 window, 2 tab
static bool in_search() { return g_searching; }
static bool g_search_running();

// Split view: two tabs side by side; clicking in a pane makes its tab current.
static bool g_split = false;
static Tab *g_pane[2] = {nullptr, nullptr}; // left and right while split
static int g_pane_focus = 0;
static Tab *tab_of(const void *view) {
  for (auto &t : g_tabs)
    if ((const void *)t->view == view) return t.get();
  return nullptr;
}
static void activate_tab(int i);
static int tab_index(const Tab *t) {
  for (size_t k = 0; k < g_tabs.size(); k++)
    if (g_tabs[k].get() == t) return (int)k;
  return -1;
}
// While the other pane's view draws or takes an event, the globals that
// describe "the current tab" (view settings, folder, search) are its tab's.
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

// ---------------------------------------------------------------------------
// The file view (icon grid or detail list)

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

// ---------------------------------------------------------------------------
// Operations

// ---- Undo (Ctrl+Z): renames, new folders and files, copies, moves and
// moving to the Trash. Deleting permanently can't be undone.
struct UndoStep {
  enum Kind { RENAME, CREATE, COPY, MOVE, TRASH } kind;
  std::vector<std::pair<std::string, std::string>> items; // from, to ("" for TRASH; CREATE: "", the new path)
};
static std::vector<UndoStep> g_undo;

static void push_undo(UndoStep::Kind k, std::vector<std::pair<std::string, std::string>> items) {
  if (items.empty()) return;
  g_undo.push_back({k, std::move(items)});
  if (g_undo.size() > 50) g_undo.erase(g_undo.begin());
}

static const char *undo_label() {
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

static void undo() {
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
static void record_op(FileOp *o) {
  if (o->kind == FileOp::DELETE) return;
  push_undo(o->kind == FileOp::COPY ? UndoStep::COPY : o->kind == FileOp::MOVE ? UndoStep::MOVE : UndoStep::TRASH,
            o->done);
}

static void run_op(FileOp::Kind k, const std::vector<std::string> &src, const std::string &dest) {
  fld::run_file_op(k, src, dest, [](FileOp *o) {
    if (g_clip.cut && o->kind == FileOp::MOVE) { g_clip.paths.clear(); g_clip.cut = false; }
    record_op(o);
    refresh();
  });
}

static void copy_to_clipboard(bool cut) {
  g_clip.paths = g_view->selected_paths();
  g_clip.cut = cut;
  std::string text;
  for (auto &p : g_clip.paths) text += "file://" + fld::uri_encode_path(p) + "\n";
  Fl::copy(text.c_str(), (int)text.size(), 1);
  g_view->redraw();
  update_status();
}

static void paste_into(const std::string &dir) {
  if (in_trash() || in_network() || dir == TRASH_URI || (in_search() && dir == g_cwd)) return;
  if (g_clip.paths.empty()) {
    Fl::paste(*g_view, 1); // maybe file URIs from another program
    return;
  }
  run_op(g_clip.cut ? FileOp::MOVE : FileOp::COPY, g_clip.paths, dir);
}

static void trash_selected(bool permanent) {
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

static void new_item(bool folder) {
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

static void properties(const std::vector<std::string> &paths_in) {
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
static void batch_rename(const std::vector<std::string> &paths) {
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

static void open_entry(Entry &e) {
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

static void open_containing(const std::string &path);
static void add_bookmark(const std::string &path);
static void update_view_button();
static void tree_follow(const std::string &path);

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

static void toggle_hidden();
static void set_view(ViewMode m);
static void set_sort(SortKey k, bool desc);
static void zoom(int delta);
static void show_preferences();
static void columns_menu(int x, int y);

static void connect_to_server();

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

// ---------------------------------------------------------------------------
// Navigation & chrome

struct Place {
  std::string label, path, icon;
  bool bookmark = false;
  std::string volume;      // UDisks2 volume ("" for other places); path is "" while unmounted
  bool can_eject = false;  // shows the eject button
  std::string uri;         // a network location (bookmark or mount); path is "" while not mounted
  std::string gvfs_mount;  // the root URI of a GVfs mount (to unmount it)
  std::string gvfs_volume; // a GVfs volume to mount (a phone, a camera)
};
static std::vector<Place> g_place_list;

static void fill_places() {
  g_place_list.clear();
  auto add = [](const std::string &l, const std::string &p, const std::string &i) {
    if (p == TRASH_URI || fld::is_dir(p)) g_place_list.push_back({l, p, i});
  };
  add("Home", fld::home_dir(), "user-home");
  add("Desktop", fld::user_dir("DESKTOP"), "user-desktop");
  add("Documents", fld::user_dir("DOCUMENTS"), "folder-documents");
  add("Downloads", fld::user_dir("DOWNLOAD"), "folder-download");
  add("Pictures", fld::user_dir("PICTURES"), "folder-pictures");
  add("Music", fld::user_dir("MUSIC"), "folder-music");
  add("Videos", fld::user_dir("VIDEOS"), "folder-videos");
  add("Trash", TRASH_URI, fld::trash_list().empty() ? "user-trash" : "user-trash-full");
  // GTK bookmarks
  std::string bm;
  if (fld::read_file(fld::config_home() + "/gtk-3.0/bookmarks", bm)) {
    for (auto &line : fld::split(bm, '\n')) {
#ifdef FLD_HAVE_GVFS
      if (gv::looks_like_uri(line)) { // a network location
        size_t sp = line.find(' ');
        std::string uri = line.substr(0, sp);
        Place p{sp == std::string::npos ? gv::display_name_for_uri(uri) : line.substr(sp + 1), "", "folder-remote"};
        p.uri = uri;
        p.bookmark = true;
        // mounted: its folder
        for (auto &m : gv::Client::get().state().mounts)
          if (!m.path.empty() && (fld::starts_with(uri, m.uri) || fld::starts_with(uri + "/", m.uri))) {
            std::string rest = uri.size() > m.uri.size() ? fld::uri_decode(uri.substr(m.uri.size())) : "";
            p.path = fld::path_join(m.path, rest);
            if (!rest.empty() && !fld::is_dir(p.path)) p.path.clear();
          }
        g_place_list.push_back(p);
        continue;
      }
#endif
      if (!fld::starts_with(line, "file://")) continue;
      size_t sp = line.find(' ');
      std::string p = fld::uri_decode(line.substr(7, sp == std::string::npos ? std::string::npos : sp - 7));
      std::string l = sp == std::string::npos ? fld::basename(p) : line.substr(sp + 1);
      add(l, p, "folder");
      if (!g_place_list.empty() && g_place_list.back().path == p) g_place_list.back().bookmark = true;
    }
  }
  add("File System", "/", "drive-harddisk");
  // drives: from UDisks2 (also unmounted ones), else the mounted file systems
  bool have_volumes = false;
#ifdef FLD_HAVE_UDISKS
  const ud::State &us = ud::Client::get().state();
  if (us.running) {
    have_volumes = true;
    for (auto &v : us.volumes) {
      Place p{v.label, v.mount_point, v.icon};
      p.volume = v.path;
      p.can_eject = v.can_eject || v.mounted();
      g_place_list.push_back(p);
    }
  }
#endif
#ifdef FLD_HAVE_GVFS
  // network locations, phones and cameras (GVfs)
  {
    const gv::State &gs = gv::Client::get().state();
    if (gs.running) {
      Place net{"Network", NETWORK_URI, "network-workgroup"};
      g_place_list.push_back(net);
    }
    for (auto &m : gs.mounts) {
      Place p{m.name, m.path, m.icon};
      p.uri = m.uri;
      p.gvfs_mount = m.uri;
      p.can_eject = m.can_unmount || m.can_eject;
      g_place_list.push_back(p);
    }
    for (auto &v : gs.volumes) {
      Place p{v.name, "", v.icon};
      p.gvfs_volume = v.id;
      g_place_list.push_back(p);
    }
  }
#endif
  if (!have_volumes) {
    std::string mounts;
    if (fld::read_file("/proc/mounts", mounts)) {
      for (auto &line : fld::split(mounts, '\n')) {
        auto f = fld::split(line, ' ');
        if (f.size() < 2) continue;
        std::string mp; // \040 escapes are octal
        for (size_t i = 0; i < f[1].size(); i++) {
          if (f[1][i] == '\\' && i + 3 < f[1].size()) { mp += (char)strtol(f[1].substr(i + 1, 3).c_str(), nullptr, 8); i += 3; }
          else mp += f[1][i];
        }
        if (fld::starts_with(mp, "/media/") || fld::starts_with(mp, "/run/media/") || fld::starts_with(mp, "/mnt/"))
          add(fld::basename(mp), mp, "drive-removable-media");
      }
    }
  }
  g_places->clear();
  for (auto &p : g_place_list) {
    g_places->add(("  " + fld::escape_at(p.label)).c_str());
    g_places->icon(g_places->size(), fld::icon_first({p.icon, "folder"}, 20));
  }
}

static std::string bookmarks_path() { return fld::config_home() + "/gtk-3.0/bookmarks"; }

// a bookmark's URI: file://..., or the network location of a remote folder
static std::string bookmark_uri(const std::string &path) {
  std::string r = remote_uri(path);
  return r.empty() ? "file://" + fld::uri_encode_path(path) : r;
}
static std::string bookmark_uri(const Place &p) { return p.uri.empty() ? bookmark_uri(p.path) : p.uri; }

static void add_bookmark_uri(const std::string &uri, const std::string &label = "") {
  std::string bm;
  fld::read_file(bookmarks_path(), bm);
  for (auto &line : fld::split(bm, '\n'))
    if (line == uri || fld::starts_with(line, uri + " ")) return; // already there
  if (!bm.empty() && bm.back() != '\n') bm += '\n';
  bm += uri + (label.empty() ? "" : " " + label) + "\n";
  fld::mkdir_p(fld::dirname(bookmarks_path()));
  if (!fld::write_file(bookmarks_path(), bm)) fl_alert("Could not save %s", bookmarks_path().c_str());
  fill_places();
  select_place_for(g_cwd);
}

static void add_bookmark(const std::string &path) {
  if (path.empty() || path == TRASH_URI || path == NETWORK_URI) return;
  // a network share is named after its location, not the gvfs-fuse folder
  std::string name = remote_mount_name(path);
  add_bookmark_uri(bookmark_uri(path), name);
}

static void remove_bookmark(const std::string &uri) {
  std::string bm, out;
  if (!fld::read_file(bookmarks_path(), bm)) return;
  for (auto &line : fld::split(bm, '\n'))
    if (line != uri && !fld::starts_with(line, uri + " ")) out += line + "\n";
  fld::write_file(bookmarks_path(), out);
  fill_places();
  select_place_for(g_cwd);
}

static void rename_bookmark(const std::string &uri, const std::string &old_label) {
  std::string label = old_label;
  if (!fld::ask_text("Rename Bookmark", "Bookmark name:", label, -1, "Rename")) return;
  const char *n = label.c_str();
  std::string bm, out;
  if (!fld::read_file(bookmarks_path(), bm)) return;
  for (auto &line : fld::split(bm, '\n')) {
    if (line == uri || fld::starts_with(line, uri + " ")) out += uri + (*n ? " " + std::string(n) : "") + "\n";
    else out += line + "\n";
  }
  fld::write_file(bookmarks_path(), out);
  fill_places();
  select_place_for(g_cwd);
}

// ---- drives (UDisks2)

// Opens a place here, in a new window or in a new tab: drives are mounted
// (and unlocked) first
enum PlaceWhere { PLACE_HERE, PLACE_WINDOW, PLACE_TAB };
static void open_place(const Place &p, PlaceWhere where = PLACE_HERE) {
  auto go = [where](const std::string &path) {
    if (where == PLACE_WINDOW) fld::spawn({"fl-files", path});
    else if (where == PLACE_TAB) new_tab(path);
    else navigate(path);
  };
#ifdef FLD_HAVE_GVFS
  if (!p.gvfs_volume.empty()) { // a phone or camera
    std::string label = p.label;
    g_status->copy_label("Mounting…");
    gv::Client::get().mount_volume(p.gvfs_volume, [go, label](const std::string &err, const std::string &path) {
      update_status();
      if (err == gv::kCancelled) return;
      if (!err.empty()) {
        fl_alert("Could not open \"%s\":\n%s", fld::escape_at(label).c_str(), fld::escape_at(err).c_str());
        return;
      }
      go(path);
    });
    return;
  }
  if (p.path.empty() && !p.uri.empty()) { // a network location that is not mounted
    mount_and_go(p.uri, where);
    return;
  }
#endif
  if (!p.path.empty() || p.volume.empty()) { go(p.path); return; }
#ifdef FLD_HAVE_UDISKS
  ud::Client &c = ud::Client::get();
  const ud::Volume *v = c.state().volume(p.volume);
  if (!v) return;
  std::string vol = v->path;
  auto mount = [go](const std::string &vol) {
    g_status->copy_label("Mounting…");
    ud::Client::get().mount(vol, [go](const std::string &err, const std::string &mp) {
      update_status();
      if (!err.empty()) { fl_alert("Could not mount the volume:\n%s", fld::escape_at(err).c_str()); return; }
      go(mp);
    });
  };
  if (v->encrypted && v->cleartext.empty()) {
    std::string q = "Passphrase for " + v->label + ":";
    const char *pw = fl_password("%s", "", fld::escape_at(q).c_str());
    if (!pw) return;
    ud::Client::get().unlock(vol, pw, [mount](const std::string &err, const std::string &clear) {
      if (!err.empty()) { fl_alert("Could not unlock the volume:\n%s", fld::escape_at(err).c_str()); return; }
      mount(clear);
    });
    return;
  }
  mount(vol);
#else
  (void)go;
#endif
}

static void unmount_place(const Place &p, bool eject) {
#ifdef FLD_HAVE_GVFS
  if (!p.gvfs_mount.empty()) {
    if (!p.path.empty()) leave_folder(p.path);
    std::string label = p.label;
    g_status->copy_label("Disconnecting…");
    gv::Client::get().unmount(p.gvfs_mount, eject, [label](const std::string &err) {
      update_status();
      if (!err.empty() && err != gv::kCancelled)
        fl_alert("Could not disconnect \"%s\":\n%s", fld::escape_at(label).c_str(), fld::escape_at(err).c_str());
    });
    return;
  }
#endif
#ifdef FLD_HAVE_UDISKS
  if (p.volume.empty()) return;
  // leave the drive first, or it is busy
  if (!p.path.empty()) leave_folder(p.path);
  std::string label = p.label;
  auto done = [label, eject](const std::string &err) {
    update_status();
    if (!err.empty())
      fl_alert("Could not %s \"%s\":\n%s", eject ? "eject" : "unmount", fld::escape_at(label).c_str(),
               fld::escape_at(err).c_str());
    else if (eject)
      g_status->copy_label(fld::escape_at("\"" + label + "\" can be removed safely.").c_str());
  };
  g_status->copy_label(eject ? "Ejecting…" : "Unmounting…");
  if (eject) ud::Client::get().eject(p.volume, done);
  else ud::Client::get().unmount(p.volume, done);
#endif
}

// Places list with a context menu
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

// ---------------------------------------------------------------------------
// Folder tree (sidebar)

static std::map<Fl_Tree_Item *, std::string> g_tree_paths;
static bool g_tree_syncing = false;

static bool has_subdirs(const std::string &dir) {
  for (auto &n : fld::list_dir(dir, S.show_hidden))
    if (fld::is_dir(fld::path_join(dir, n))) return true;
  return false;
}

static void tree_forget(Fl_Tree_Item *it) {
  for (int i = 0; i < it->children(); i++) {
    tree_forget(it->child(i));
    g_tree_paths.erase(it->child(i));
  }
}

static Fl_Tree_Item *tree_add(Fl_Tree_Item *parent, const std::string &label, const std::string &path) {
  Fl_Tree_Item *it = g_tree->add(parent, fld::escape_at(label).c_str());
  g_tree_paths[it] = path;
  it->usericon(fld::icon_first({path == fld::home_dir() ? "user-home" : path == "/" ? "drive-harddisk" : "folder"}, 16));
  if (has_subdirs(path)) g_tree->add(it, "…"); // placeholder: shows the expander
  it->close();
  return it;
}

// The folder tree accepts drops onto its folders
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

// fills an item with its subfolders (on first open)
static void tree_populate(Fl_Tree_Item *it) {
  auto pit = g_tree_paths.find(it);
  if (pit == g_tree_paths.end()) return;
  std::string dir = pit->second;
  tree_forget(it);
  g_tree->clear_children(it);
  std::vector<std::string> names;
  for (auto &n : fld::list_dir(dir, S.show_hidden))
    if (fld::is_dir(fld::path_join(dir, n))) names.push_back(n);
  std::sort(names.begin(), names.end(), [](auto &a, auto &b) { return fld::natural_compare(a, b) < 0; });
  for (auto &n : names) tree_add(it, n, fld::path_join(dir, n));
}

static void tree_build() {
  g_tree->clear_children(g_tree->root()); // clear() would delete the root item too
  g_tree_paths.clear();
  tree_add(g_tree->root(), "Home", fld::home_dir());
  tree_add(g_tree->root(), "File System", "/");
}

// opens the tree down to `path` and selects it
static void tree_follow(const std::string &path) {
  if (!g_tree || !g_tree->visible() || path == TRASH_URI || path == NETWORK_URI || in_search()) return;
  Fl_Tree_Item *base = nullptr;
  std::string rest;
  std::string home = fld::home_dir();
  for (int i = 0; i < g_tree->root()->children(); i++) {
    Fl_Tree_Item *c = g_tree->root()->child(i);
    const std::string &cp = g_tree_paths[c];
    if (path == cp || (cp != "/" && fld::starts_with(path, cp + "/"))) { base = c; rest = path.substr(cp.size()); break; }
  }
  if (!base) {
    base = g_tree->root()->child(1);
    rest = path;
  }
  Fl_Tree_Item *cur = base;
  for (auto &comp : fld::split(rest, '/')) {
    if (comp.empty()) continue;
    if (cur->children() == 1 && g_tree_paths.find(cur->child(0)) == g_tree_paths.end()) tree_populate(cur);
    g_tree_syncing = true;
    g_tree->open(cur, 0);
    g_tree_syncing = false;
    Fl_Tree_Item *next = nullptr;
    for (int i = 0; i < cur->children(); i++)
      if (fld::basename(g_tree_paths[cur->child(i)]) == comp) next = cur->child(i);
    if (!next) break;
    cur = next;
  }
  g_tree_syncing = true;
  g_tree->select_only(cur, 0);
  g_tree->show_item_middle(cur);
  g_tree_syncing = false;
  g_tree->redraw();
}

static void tree_cb(Fl_Widget *, void *) {
  if (g_tree_syncing) return;
  Fl_Tree_Item *it = g_tree->callback_item();
  if (!it) return;
  switch (g_tree->callback_reason()) {
    case FL_TREE_REASON_OPENED:
      if (it->children() == 1 && g_tree_paths.find(it->child(0)) == g_tree_paths.end()) tree_populate(it);
      break;
    case FL_TREE_REASON_SELECTED: {
      auto p = g_tree_paths.find(it);
      if (p != g_tree_paths.end() && p->second != g_cwd) navigate(p->second);
      break;
    }
    default: break;
  }
}

static void show_tree(bool on) {
  S.show_tree = on;
  save_settings();
  if (on) {
    if (g_tree->root()->children() == 0) tree_build();
    g_tree->show();
    g_places->hide();
    tree_follow(g_cwd);
  } else {
    g_tree->hide();
    g_places->show();
  }
}

static void select_place_for(const std::string &path) {
  int best = 0;
  for (size_t i = 0; i < g_place_list.size(); i++)
    if (g_place_list[i].path == path) best = (int)i + 1;
  g_places->value(best);
}

static void watch_dir(const std::string &dir) {
  if (g_inotify < 0) return;
  if (g_watch >= 0) inotify_rm_watch(g_inotify, g_watch);
  std::string d = dir == TRASH_URI ? fld::trash_dir() + "/files" : dir;
  g_watch = inotify_add_watch(g_inotify, d.c_str(),
                              IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE);
}

static void update_nav_buttons() {
  if (g_hist_back.empty() && !in_search()) g_back->deactivate(); else g_back->activate();
  if (g_hist_fwd.empty()) g_fwd->deactivate(); else g_fwd->activate();
  if (g_cwd == "/" || in_trash() || in_network()) g_up->deactivate(); else g_up->activate();
  if (in_trash()) g_empty_trash->show(); else g_empty_trash->hide();
}

// The name of a tab's folder (or search) for the title and the tab
static std::string location_name(const std::string &cwd, bool searching, const std::string &desc) {
  if (searching) return "Search: " + desc;
  if (cwd == TRASH_URI) return "Trash";
  if (cwd == NETWORK_URI) return "Network";
  std::string mount = remote_mount_name(cwd);
  if (!mount.empty()) return mount;
  if (cwd == "/" || cwd.empty()) return "/";
  return fld::basename(cwd);
}

// The path field, window title and selected place for the current tab
static void show_location() {
  // network folders show their location (sftp://host/dir), not the gvfs-fuse path
  std::string uri = remote_uri(g_cwd);
  g_path->value(g_cwd == TRASH_URI ? "Trash" : g_cwd == NETWORK_URI ? "Network" : !uri.empty() ? uri.c_str() : g_cwd.c_str());
  g_win->copy_label((location_name(g_cwd, g_searching, g_search_desc) + (geteuid() == 0 ? " – Files (Administrator)" : " – Files")).c_str());
  if (g_searching) {
    g_places->value(0);
    if (g_tree) g_tree->deselect_all(nullptr, 0);
  } else select_place_for(g_cwd);
}

static void network_views_reload();

#ifdef FLD_HAVE_MDNS
static mdns::Browser *g_mdns;

static void start_network_discovery() {
  if (g_mdns) return;
  static const std::vector<std::string> types = {"_smb._tcp", "_sftp-ssh._tcp", "_webdav._tcp", "_webdavs._tcp",
                                                 "_ftp._tcp", "_nfs._tcp"};
  auto to_entry = [](const mdns::Service &s) {
    Entry e;
    // "Server" for Windows shares (as GVfs shows them), "Server (SFTP)" for the rest
    e.name = s.type == "_smb._tcp" ? s.name : s.name + " (" + mdns::type_label(s.type) + ")";
    e.path = e.target = mdns::service_url(s);
    e.icon_name = s.type == "_smb._tcp" ? "network-server" : "folder-remote";
    e.is_dir = true;
    e.mime = "inode/directory";
    return e;
  };
  g_mdns = new mdns::Browser(types, [to_entry](const mdns::Service &s) {
    Entry e = to_entry(s);
    if (e.target.empty()) return;
    for (auto &x : g_mdns_entries)
      if (x.target == e.target) return;
    g_mdns_names[e.target] = s.name; // GVfs lists the same server by this name
    g_mdns_entries.push_back(e);
    network_views_reload();
  }, [to_entry](const mdns::Service &s) {
    std::string t = mdns::service_url(s);
    for (size_t i = 0; i < g_mdns_entries.size(); i++)
      if (g_mdns_entries[i].target == t) {
        g_mdns_entries.erase(g_mdns_entries.begin() + (long)i);
        network_views_reload();
        return;
      }
  });
}
#endif

// the tabs that show the network
static void network_views_reload() {
  for (auto &t : g_tabs) {
    bool cur = t.get() == cur_tab();
    if ((cur ? g_cwd : t->cwd) != NETWORK_URI || (cur ? g_searching : t->searching)) continue;
    t->view->load(NETWORK_URI);
    if (cur) update_status();
  }
}

// Lists the network neighbourhood again; the network view shows it when done
static void network_relist() {
#ifdef FLD_HAVE_MDNS
  start_network_discovery();
#endif
#ifdef FLD_HAVE_GVFS
  if (g_net_listing) return;
  g_net_listing = true;
  g_net_error.clear();
  gv::Client::get().list(NETWORK_URI, [](const std::string &err, std::vector<gv::NetEntry> list) {
    g_net_listing = false;
    g_net_error = err == gv::kCancelled ? "" : err;
    g_net_entries.clear();
    for (auto &n : list) {
      Entry e;
      e.name = n.name;
      e.path = e.target = n.target;
      e.icon_name = n.icon;
      e.is_dir = true;
      e.mime = "inode/directory";
      g_net_entries.push_back(e);
    }
    network_views_reload();
  });
  if (g_view) g_view->redraw();
#else
  g_net_error = "fl-files was built without GVfs support";
#endif
}

static void navigate(const std::string &path_in, bool record) {
  std::string path = path_in == TRASH_URI || path_in == NETWORK_URI ? path_in : fld::abs_path(path_in);
  if (path == NETWORK_URI) network_relist();
  else if (path != TRASH_URI) {
    if (!fld::is_dir(path)) {
      fl_alert("\"%s\" is not a folder.", path.c_str());
      return;
    }
    if (access(path.c_str(), R_OK | X_OK) != 0) {
      fl_alert("You don't have permission to open \"%s\".", path.c_str());
      return;
    }
  }
  bool was_search = in_search();
  if (was_search) {
    g_search->cancel();
    g_searching = false;
  }
  std::string prev = was_search ? "" : g_cwd;
  if (record && !was_search && !g_cwd.empty() && g_cwd != path) {
    g_hist_back.push_back(g_cwd);
    if (g_hist_back.size() > 100) g_hist_back.pop_front();
    g_hist_fwd.clear();
  }
  g_cwd = path;
  g_filter->value("");
  ViewState nv = folder_view(path);
  if (nv.zoom != V.zoom) g_view->clear_images();
  V = nv;
  update_view_button();
  update_zoom_slider();
  // when going up, select the folder we came from
  std::string select;
  if (!prev.empty() && prev != TRASH_URI && fld::dirname(prev) == path) select = fld::basename(prev);
  g_view->load(path, select);
  show_location();
  tree_follow(path);
  watch_dir(path);
  update_nav_buttons();
  update_status();
  update_tabbar();
  g_view->take_focus();
}

static void refresh() {
  if (in_search()) {
    g_view->restat_all();
    update_status();
    return;
  }
  if (in_network()) network_relist();
#ifdef FLD_HAVE_GVFS
  else if (is_remote(g_cwd)) gv::Client::get().refresh(); // a share may have gone
#endif
  std::set<std::string> selected;
  for (int i : g_view->sel) selected.insert(g_view->entry(i).name);
  int top = g_view->top;
  g_view->load(g_cwd);
  for (int i = 0; i < (int)g_view->items.size(); i++)
    if (selected.count(g_view->entry(i).name)) g_view->sel.insert(i);
  g_view->top = top;
  g_view->update_scrollbar();
  fill_places();
  select_place_for(g_cwd);
  update_status();
}

static void update_status() {
  if (!g_status) return;
  if (g_view->dragging_files) {
    size_t n = (size_t)g_view->drag_count;
    std::string what = std::to_string(n) + (n == 1 ? " item" : " items");
    int st = Fl::event_state();
    std::string s;
    int t = g_view->drop_item;
    if (t >= 0 && t < (int)g_view->items.size() && !g_view->sel.empty()) {
      fld::DropAction a = fld::drop_action(g_view->selected_paths(), g_view->entry(t).path, st);
      const char *verb = a == fld::DROP_COPY ? "Copy " : a == fld::DROP_MOVE ? "Move " : nullptr;
      s = verb ? std::string(verb) + what + " to “" + g_view->entry(t).name + "”" +
                     (a == fld::DROP_COPY ? "" : " (Ctrl copies)")
               : "Cannot drop here";
    } else
      s = "Dragging " + what + " — drop onto a folder to move or copy it there (Ctrl copies, Shift moves)";
    g_status->copy_label(fld::escape_at(s).c_str());
    return;
  }
  std::string s = std::to_string(g_view->items.size()) + " item" + (g_view->items.size() == 1 ? "" : "s");
  if (in_search()) {
    size_t n = g_view->items.size();
    if (g_search->running())
      s = "Searching " + fld::tilde(g_search->current_dir()) + "…    " + std::to_string(n) + " found";
    else
      s = std::to_string(n) + (n == 1 ? " match" : " matches") + " in " + fld::tilde(g_cwd) +
          (g_search->truncated() ? " (stopped at the result limit)" : "") + "    " +
          std::to_string(g_search->scanned()) + " items searched";
  }
  if (!g_view->sel.empty()) {
    unsigned long long bytes = 0;
    for (int i : g_view->sel) {
      Entry &e = g_view->entry(i);
      if (!e.is_dir) bytes += e.size;
    }
    s += "    " + std::to_string(g_view->sel.size()) + " selected";
    if (bytes) s += " (" + fld::human_size(bytes) + ")";
    if (g_view->sel.size() == 1) {
      Entry &e = g_view->entry(*g_view->sel.begin());
      s += "    " + fld::mime_description(e.get_mime());
    }
  }
  if (!g_clip.paths.empty())
    s += "    " + std::to_string(g_clip.paths.size()) + (g_clip.cut ? " cut" : " copied");
  if (!in_trash()) {
    struct statvfs vf;
    if (statvfs(g_cwd.c_str(), &vf) == 0 && vf.f_blocks) // FTP and some shares have no sizes
      s += "    Free: " + fld::human_size((unsigned long long)vf.f_bavail * vf.f_frsize);
  }
  g_status->copy_label(fld::escape_at(s).c_str());
}

static void go_back() {
  if (in_search()) { navigate(g_cwd, false); return; }
  if (g_hist_back.empty()) return;
  g_hist_fwd.push_back(g_cwd);
  std::string p = g_hist_back.back();
  g_hist_back.pop_back();
  navigate(p, false);
}

static void go_forward() {
  if (g_hist_fwd.empty()) return;
  g_hist_back.push_back(g_cwd);
  std::string p = g_hist_fwd.back();
  g_hist_fwd.pop_back();
  navigate(p, false);
}

static void go_up() {
  if (in_search()) { navigate(g_cwd, false); return; }
  if (in_trash() || in_network() || g_cwd == "/") return;
  // up from a network location's top folder: the network
  if (!remote_mount_name(g_cwd).empty()) { navigate(NETWORK_URI); return; }
  navigate(fld::dirname(g_cwd));
}

static void toggle_hidden() {
  S.show_hidden = !S.show_hidden;
  save_settings();
  g_view->apply_filter_sort();
  if (g_tree && g_tree->visible()) {
    tree_build();
    tree_follow(g_cwd);
  }
  update_status();
}

// ---------------------------------------------------------------------------
// Find files and folders

static Fl_Group *g_sbar;
static Fl_Input *g_s_name, *g_s_text;
static Fl_Choice *g_s_kind, *g_s_date, *g_s_size;
static Fl_Check_Button *g_s_hidden;
static Fl_Button *g_s_go;
static Fl_Tile *g_tile;
static bool g_s_case = false, g_s_regex = false, g_s_links = false;
static const int SBAR_H = 42;

// takes the results found so far into the view of the tab that searches
static void search_poll(void *d) {
  Tab *t = (Tab *)d;
  bool cur = t == cur_tab();
  std::vector<std::string> paths;
  t->search->take(paths);
  if (!paths.empty()) t->view->add_paths(paths);
  if (cur) update_status();
  t->view->redraw();
  if (t->search->running()) Fl::repeat_timeout(0.15, search_poll, d);
  else {
    if (cur) g_s_go->label("Search");
    // the last batch
    paths.clear();
    t->search->take(paths);
    if (!paths.empty()) t->view->add_paths(paths);
    if (cur) update_status();
  }
}

static void stop_search() {
  g_search->cancel();
  Fl::remove_timeout(search_poll, cur_tab());
  search_poll(cur_tab());
}

static void start_search() {
  if (g_search->running()) {
    stop_search();
    return;
  }
  search::Query q;
  q.root = g_cwd == TRASH_URI || g_cwd == NETWORK_URI ? fld::home_dir() : g_cwd;
  q.name = fld::trim(g_s_name->value());
  q.content = g_s_text->value();
  q.kind = (search::Query::Kind)std::max(0, g_s_kind->value());
  static const int days[] = {0, 1, 7, 30, 365};
  int di = std::max(0, g_s_date->value());
  q.newer_than = days[di] ? time(nullptr) - days[di] * 86400L : 0;
  switch (g_s_size->value()) {
    case 1: q.max_size = 100 << 10; break;
    case 2: q.min_size = 100 << 10; q.max_size = 10 << 20; break;
    case 3: q.min_size = 10 << 20; break;
    case 4: q.min_size = 100 << 20; break;
    case 5: q.min_size = 1ll << 30; break;
  }
  if (!q.content.empty() || q.min_size >= 0 || q.max_size >= 0) {
    if (q.kind == search::Query::FOLDERS) {
      fl_alert("Folders have no contents or size to search for; choose Files.");
      return;
    }
    q.kind = search::Query::FILES;
  }
  q.hidden = g_s_hidden->value();
  q.match_case = g_s_case;
  q.name_regex = g_s_regex;
  q.follow_links = g_s_links;
  if (q.name.empty() && q.content.empty() && !q.newer_than && q.min_size < 0 && q.max_size < 0) {
    fl_beep();
    g_s_name->take_focus();
    return;
  }
  if (!g_search->start(q)) {
    fl_alert("The regular expression is not valid.");
    return;
  }
  if (!in_search()) {
    if (g_cwd == TRASH_URI || g_cwd == NETWORK_URI) g_cwd = q.root;
    g_searching = true;
  }
  g_search_desc = q.name.empty() ? (q.content.empty() ? "files" : "\"" + q.content + "\"") : q.name;
  g_view->clear_thumbs();
  g_view->all.clear();
  g_filter->value("");
  g_view->apply_filter_sort();
  show_location();
  g_s_go->label("Stop");
  update_nav_buttons();
  Fl::remove_timeout(search_poll, cur_tab());
  Fl::add_timeout(0.1, search_poll, cur_tab());
  update_status();
  update_tabbar();
}

// Places the bars above the views: the toolbar, the tab bar (with more
// than one tab, or always), the find bar
static fld::Toolbar *g_toolbar;
static Fl_Menu_Bar *g_menubar;
static void rebuild_menubar(); // its menu titles, before it shows
static const int TOOLBAR_H = 42, STATUS_H = 24, MENUBAR_H = 26;
static void relayout() {
  int W = g_win->w(), H = g_win->h(), y = 0;
  if (S.menubar) {
    if (!g_menubar->visible()) rebuild_menubar();
    g_menubar->resize(0, 0, W, MENUBAR_H);
    g_menubar->show();
    y += MENUBAR_H;
  } else g_menubar->hide();
  g_toolbar->resize(0, y, W, TOOLBAR_H);
  y += TOOLBAR_H;
  bool tabs = g_tabs.size() > 1 || S.always_tabs;
  if (tabs) {
    int th = fld::TabBar::preferred_height();
    g_tabbar->resize(0, y, W, th);
    g_tabbar->show();
    y += th;
  } else g_tabbar->hide();
  if (g_sbar->visible()) {
    g_sbar->resize(0, y, W, SBAR_H);
    y += SBAR_H;
  }
  g_tile->resize(0, y, W, H - y - STATUS_H);
  g_tile->init_sizes();
  g_win->init_sizes();
  g_win->redraw();
}

static void show_search_bar(bool on) {
  if (on == (bool)g_sbar->visible()) {
    if (on) g_s_name->take_focus();
    return;
  }
  if (on) {
    g_sbar->show();
    relayout();
    g_s_name->take_focus();
    g_s_name->insert_position(0, g_s_name->size());
  } else {
    if (in_search()) navigate(g_cwd, false);
    g_sbar->hide();
    relayout();
    g_view->take_focus();
  }
}

static void open_containing(const std::string &path) {
  navigate(fld::dirname(path));
  std::string name = fld::basename(path);
  for (int i = 0; i < (int)g_view->items.size(); i++)
    if (g_view->entry(i).name == name) {
      g_view->select_only(i);
      break;
    }
}

// The find bar; Enter in any field searches, Escape closes it
class SearchBar : public Fl_Group {
public:
  using Fl_Group::Fl_Group;
  int handle(int e) override {
    if (e == FL_KEYBOARD || e == FL_SHORTCUT) {
      int k = Fl::event_key();
      if ((k == FL_Enter || k == FL_KP_Enter) && contains(Fl::focus())) {
        start_search();
        return 1;
      }
      if (k == FL_Escape && contains(Fl::focus())) {
        if (g_search->running()) stop_search();
        else show_search_bar(false);
        return 1;
      }
    }
    return Fl_Group::handle(e);
  }
};

static void build_search_bar(int W, int Y) {
  const fld::Palette &p = fld::palette();
  g_sbar = new SearchBar(0, Y, W, SBAR_H);
  g_sbar->box(FL_FLAT_BOX);
  g_sbar->color(p.bg);
  int y = Y + 7, x = 8;
  g_s_name = new Fl_Input(x + 46, y, 170, 28, "Name:");
  g_s_name->tooltip("Part of the name; * and ? are wildcards (e.g. *.pdf)");
  x += 46 + 176;
  g_s_text = new Fl_Input(x + 64, y, 130, 28, "Contains:");
  g_s_text->tooltip("Only files that contain this text (text files up to 64 MiB)");
  x += 64 + 136;
  g_s_kind = new Fl_Choice(x, y, 108, 28);
  g_s_kind->add("All items");
  g_s_kind->add("Files");
  g_s_kind->add("Folders");
  g_s_kind->value(0);
  x += 114;
  g_s_date = new Fl_Choice(x, y, 116, 28);
  for (const char *t : {"Any date", "Last day", "Last week", "Last month", "Last year"}) g_s_date->add(t);
  g_s_date->value(0);
  g_s_date->tooltip("Modified within");
  x += 122;
  g_s_size = new Fl_Choice(x, y, 104, 28);
  for (const char *t : {"Any size", "Under 100 KiB", "100 KiB–10 MiB", "Over 10 MiB", "Over 100 MiB", "Over 1 GiB"})
    g_s_size->add(t);
  g_s_size->value(0);
  x += 110;
  g_s_hidden = new fld::CheckButton(x, y, 76, 28, "Hidden");
  g_s_hidden->tooltip("Also search hidden files and folders");
  x += 78;
  Fl_Menu_Button *opts = new Fl_Menu_Button(x, y, 34, 28, "⋯");
  opts->tooltip("Search options");
  opts->add("Match case", 0, [](Fl_Widget *, void *) { g_s_case = !g_s_case; }, nullptr, FL_MENU_TOGGLE);
  opts->add("Regular expression", 0, [](Fl_Widget *, void *) { g_s_regex = !g_s_regex; }, nullptr, FL_MENU_TOGGLE);
  opts->add("Follow symbolic links", 0, [](Fl_Widget *, void *) { g_s_links = !g_s_links; }, nullptr, FL_MENU_TOGGLE);
  x += 40;
  g_s_go = new Fl_Button(x, y, 76, 28, "Search");
  g_s_go->callback([](Fl_Widget *, void *) { start_search(); });
  x += 82;
  fld::FlatButton *close = new fld::FlatButton(x, y, 28, 28, "×");
  close->color(p.bg);
  close->tooltip("Close the find bar (Escape)");
  close->callback([](Fl_Widget *, void *) { show_search_bar(false); });
  Fl_Box *rest = new Fl_Box(x + 30, y, 2, 28);
  g_sbar->resizable(rest);
  g_sbar->end();
  g_sbar->hide();
}

static void update_view_button() {
  // the button shows the view it switches to
  const char *next = V.view == VIEW_DETAILS ? "view-grid" : "view-list";
  g_view_btn->icon_name(next, 20);
  if (!fld::icon(next, 20)) g_view_btn->icon_name((std::string(next) + "-symbolic").c_str(), 20);
}

static void set_view(ViewMode m) {
  V.view = m;
  remember_view(in_search() ? "" : g_cwd);
  update_view_button();
  g_view->top = 0;
  g_view->update_scrollbar();
  g_view->redraw();
}

static void set_sort(SortKey k, bool desc) {
  V.sort = k;
  V.desc = desc;
  remember_view(in_search() ? "" : g_cwd);
  g_view->resort_keep_selection();
}

// delta: +1 / -1, or 0 for the default size
static void update_zoom_slider();
static void set_zoom(int z) {
  z = std::max(0, std::min(kZoomMax, z));
  if (z == V.zoom) return;
  V.zoom = z;
  remember_view(in_search() ? "" : g_cwd);
  g_view->clear_images();
  int f = g_view->focus;
  g_view->update_scrollbar();
  g_view->ensure_visible(f);
  g_view->redraw();
  update_zoom_slider();
}
static void zoom(int delta) { set_zoom(delta == 0 ? kZoomDefault : V.zoom + delta); }

static void update_zoom_slider() {
  if (!g_zoom_slider) return;
  g_zoom_slider->value(V.zoom);
  static char tip[32];
  snprintf(tip, sizeof tip, "Icon size: %d px", kZoomPx[V.zoom]);
  g_zoom_slider->tooltip(tip);
}

static void columns_menu(int x, int y) {
  std::vector<Fl_Menu_Item> m;
  for (int c = 0; c < NUM_COLS; c++) {
    if (c == COL_LOCATION) continue; // shown for search results
    m.push_back({kColNames[c], 0, nullptr, (void *)(long)c, FL_MENU_TOGGLE | (S.col_visible[c] ? FL_MENU_VALUE : 0)});
  }
  m.back().flags |= FL_MENU_DIVIDER;
  m.push_back({"Reset Widths", 0, nullptr, (void *)100});
  m.push_back({nullptr});
  const Fl_Menu_Item *r = m[0].popup(x, y);
  if (!r) return;
  long id = (long)r->user_data();
  if (id == 100) {
    const int def[NUM_COLS] = {90, 160, 150, 100, 100, 260};
    for (int c = 0; c < NUM_COLS; c++) S.col_w[c] = def[c];
  } else {
    S.col_visible[id] = !S.col_visible[id];
  }
  save_settings();
  if (V.view != VIEW_DETAILS) set_view(VIEW_DETAILS);
  g_view->redraw();
}

static void show_preferences() {
  Fl_Double_Window *w = new Fl_Double_Window(500, 430, "Files Preferences");
  int y = 14;
  auto heading = [&](const char *t) {
    Fl_Box *b = new Fl_Box(16, y, 428, 22, t);
    b->labelfont(FL_HELVETICA_BOLD);
    b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    y += 26;
  };
  auto check = [&](const char *t, bool v) {
    Fl_Check_Button *c = new fld::CheckButton(28, y, 416, 24, t);
    c->value(v);
    y += 26;
    return c;
  };
  heading("Views");
  Fl_Choice *view = new Fl_Choice(200, y, 160, 26, "Default view:");
  view->add("Icons");
  view->add("Compact");
  view->add("Details");
  view->value(S.def.view);
  y += 32;
  Fl_Choice *date = new Fl_Choice(200, y, 244, 26, "Date format:");
  time_t now = time(nullptr) - 3600;
  for (int i = 0; i < 5; i++) date->add(fld::escape_at(fld::format_time(now, kDateFormats[i])).c_str());
  date->add("Relative (Today, Yesterday…)");
  date->value(S.date_fmt);
  y += 34;
  Fl_Check_Button *per = check("Remember the view and sort order of each folder", S.per_folder);
  Fl_Check_Button *ff = check("Sort folders before files", S.folders_first);
  Fl_Check_Button *hid = check("Show hidden files", S.show_hidden);
  y += 6;
  heading("Behavior");
  Fl_Check_Button *one = check("Open items with a single click", S.single_click);
  Fl_Check_Button *conf = check("Ask before moving items to the Trash", S.confirm_trash);
  Fl_Check_Button *mtab = check("Middle click opens folders in a new tab (else in a new window)", S.middle_tab);
  Fl_Check_Button *atabs = check("Always show the tab bar", S.always_tabs);
  y += 6;
  heading("Thumbnails");
  Fl_Check_Button *th = check("Show image thumbnails", S.thumbnails);
  Fl_Check_Button *rth = check("Also for files on network locations (slower)", S.remote_thumbs);
  Fl_Spinner *tmax = new Fl_Spinner(200, y, 80, 26, "Only for files up to:");
  tmax->range(1, 1024);
  tmax->value(S.thumb_max_mb);
  Fl_Box *mib = new Fl_Box(286, y, 40, 26, "MiB");
  mib->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  y += 40;
  Fl_Button *forget = new Fl_Button(16, y, 200, 30, "Forget Folder Views");
  forget->tooltip("Use the default view in every folder again");
  forget->callback([](Fl_Widget *, void *) {
    unlink(folder_settings_path().c_str());
    fl_message("Folders use the default view again.");
  });
  Fl_Return_Button *ok = new Fl_Return_Button(344, y, 100, 30, "Close");
  ok->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  w->size(500, y + 44);
  w->end();
  w->set_modal();
  w->show();
  while (w->shown()) Fl::wait();
  bool reload = S.show_hidden != (bool)hid->value() || S.folders_first != (bool)ff->value() ||
                S.thumbnails != (bool)th->value() || S.thumb_max_mb != (int)tmax->value() ||
                S.remote_thumbs != (bool)rth->value();
  bool view_changed = S.def.view != (ViewMode)view->value();
  S.def.view = (ViewMode)view->value();
  S.date_fmt = date->value();
  S.per_folder = per->value();
  S.folders_first = ff->value();
  S.show_hidden = hid->value();
  S.single_click = one->value();
  S.confirm_trash = conf->value();
  S.middle_tab = mtab->value();
  bool tabs_changed = S.always_tabs != (bool)atabs->value();
  S.always_tabs = atabs->value();
  S.thumbnails = th->value();
  S.remote_thumbs = rth->value();
  S.thumb_max_mb = (int)tmax->value();
  save_settings();
  Fl::delete_widget(w);
  if (view_changed && !S.per_folder) set_view(S.def.view);
  else if (view_changed && !in_search() && folder_view(g_cwd).view == S.def.view) set_view(S.def.view);
  if (reload) refresh();
  if (tabs_changed) relayout();
  g_view->redraw();
}

static void refresh_later(void *) { refresh(); }

static void inotify_cb(int fd, void *) {
  char buf[8192];
  while (read(fd, buf, sizeof buf) > 0) {}
  Fl::remove_timeout(refresh_later);
  Fl::add_timeout(0.25, refresh_later);
}

// ---------------------------------------------------------------------------
// Tabs

// stores the current tab's state (the globals) in its Tab
static void save_tab() {
  Tab *t = cur_tab();
  if (!t) return;
  t->cwd = g_cwd;
  t->back = std::move(g_hist_back);
  t->fwd = std::move(g_hist_fwd);
  t->searching = g_searching;
  t->search_desc = g_search_desc;
  t->v = V;
  t->filter = g_filter->value();
  t->find_bar = g_sbar->visible();
  t->view->bg_search = g_searching;
  t->view->bg_trash = in_trash();
  t->view->bg_filter = t->filter;
  g_hist_back.clear();
  g_hist_fwd.clear();
}

// shows the current tab's view, or both panes in a split
static void place_views() {
  int X = g_views->x(), Y = g_views->y(), W = g_views->w(), H = g_views->h();
  if (!g_split) {
    g_view->resize(X, Y, W, H);
    g_view->show();
    return;
  }
  int half = (W - 2) / 2;
  for (int k = 0; k < 2; k++) {
    if (!g_pane[k]) continue;
    FileView *v = g_pane[k]->view;
    if (k == 0) v->resize(X, Y, half, H);
    else v->resize(X + half + 2, Y, W - half - 2, H);
    v->show();
  }
  g_views->redraw();
}

static void new_tab(const std::string &path, bool activate);
static std::string new_tab_dir();

// only the current pane's folder is watched: the other one is looked at every second
static void split_poll(void *) {
  if (!g_split) return;
  Tab *o = g_pane[1 - g_pane_focus];
  if (o && !o->searching && o->cwd != NETWORK_URI) {
    std::string d = o->cwd == TRASH_URI ? fld::trash_dir() + "/files" : o->cwd;
    struct stat st;
    if (stat(d.c_str(), &st) == 0 && (st.st_mtim.tv_sec != o->view->loaded_mtime.tv_sec ||
                                      st.st_mtim.tv_nsec != o->view->loaded_mtime.tv_nsec)) {
      PaneGuard guard(o->view);
      int top = o->view->top;
      o->view->load(o->cwd);
      o->view->top = std::max(0, std::min(top, o->view->content_height() - o->view->view_h()));
      o->view->redraw();
    }
  }
  Fl::repeat_timeout(1.0, split_poll);
}

// F4: two folders side by side (the current tab and the next one, or a new tab)
static void toggle_split() {
  if (g_split) {
    Fl::remove_timeout(split_poll);
    g_split = false;
    for (Tab *t : g_pane)
      if (t && t->view != g_view) t->view->hide();
    g_pane[0] = g_pane[1] = nullptr;
    place_views();
    g_view->take_focus();
    return;
  }
  Tab *cur = cur_tab();
  int other = g_cur + 1 < (int)g_tabs.size() ? g_cur + 1 : g_cur - 1;
  if (other < 0) {
    new_tab(new_tab_dir(), false);
    other = g_cur + 1;
  }
  g_split = true;
  g_pane[0] = cur;
  g_pane[1] = g_tabs[(size_t)other].get();
  g_pane_focus = 0;
  place_views();
  g_view->take_focus();
  Fl::add_timeout(1.0, split_poll);
}

// the other pane's folder (for copying and moving between panes), or ""
static std::string other_pane_dir() {
  if (!g_split) return "";
  Tab *o = g_pane[1 - g_pane_focus];
  if (!o || o->searching || o->cwd == TRASH_URI || o->cwd == NETWORK_URI) return "";
  return o->cwd;
}

static void activate_tab_(int i, bool save) {
  if (i < 0 || i >= (int)g_tabs.size()) return;
  if (save) {
    if (i == g_cur) return;
    save_tab();
  }
  if (g_view) {
    if (!g_split) g_view->hide();
    Fl::remove_idle(FileView::thumb_idle, g_view);
  }
  FileView *was = g_view;
  g_cur = i;
  Tab *t = cur_tab();
  if (g_split) {
    if (g_pane[0] == t) g_pane_focus = 0;
    else if (g_pane[1] == t) g_pane_focus = 1;
    else {
      if (g_pane[g_pane_focus]) g_pane[g_pane_focus]->view->hide();
      g_pane[g_pane_focus] = t;
    }
  }
  g_view = t->view;
  g_cwd = t->cwd;
  g_hist_back = std::move(t->back);
  g_hist_fwd = std::move(t->fwd);
  g_searching = t->searching;
  g_search = t->search.get();
  g_search_desc = t->search_desc;
  V = t->v;
  g_filter->value(t->filter.c_str());
  place_views();
  if (was && was != g_view) was->redraw();
  if (t->find_bar != (bool)g_sbar->visible()) {
    if (t->find_bar) g_sbar->show(); else g_sbar->hide();
  }
  g_s_go->label(g_search->running() ? "Stop" : "Search");
  update_view_button();
  update_zoom_slider();
  show_location();
  tree_follow(g_cwd);
  watch_dir(g_cwd);
  update_nav_buttons();
  // the folder may have changed while the tab was in the background
  if (!g_searching && !g_cwd.empty()) {
    struct stat st;
    std::string d = g_cwd == TRASH_URI ? fld::trash_dir() + "/files" : g_cwd;
    if (stat(d.c_str(), &st) == 0 && (st.st_mtim.tv_sec != g_view->loaded_mtime.tv_sec ||
                                      st.st_mtim.tv_nsec != g_view->loaded_mtime.tv_nsec))
      refresh();
  }
  update_status();
  update_tabbar();
  relayout();
  g_view->take_focus();
}

static void activate_tab(int i) { activate_tab_(i, true); }

static void update_tabbar() {
  if (!g_tabbar) return;
  std::vector<fld::TabInfo> tabs;
  for (size_t k = 0; k < g_tabs.size(); k++) {
    Tab *t = g_tabs[k].get();
    bool cur = (int)k == g_cur;
    const std::string &cwd = cur ? g_cwd : t->cwd;
    bool searching = cur ? g_searching : t->searching;
    fld::TabInfo ti;
    ti.label = location_name(cwd, searching, cur ? g_search_desc : t->search_desc);
    std::string ru = remote_uri(cwd);
    ti.tooltip = cwd == TRASH_URI ? "Trash" : cwd == NETWORK_URI ? "Network" : !ru.empty() ? ru : fld::tilde(cwd);
    ti.busy = t->search->running();
    tabs.push_back(ti);
  }
  g_tabbar->set_tabs(tabs, g_cur);
  bool want = g_tabs.size() > 1 || S.always_tabs;
  if (want != (bool)g_tabbar->visible()) relayout();
}

static void new_tab(const std::string &path, bool activate) {
  if (path != TRASH_URI && path != NETWORK_URI && !fld::is_dir(path)) return;
  auto t = std::make_unique<Tab>();
  Fl_Group::current(nullptr);
  t->view = new FileView(g_views->x(), g_views->y(), g_views->w(), g_views->h());
  t->view->hide();
  g_views->add(t->view);
  t->v = folder_view(path);
  int at = g_cur + 1; // after the current tab
  g_tabs.insert(g_tabs.begin() + at, std::move(t));
  if (g_cur >= at) g_cur++;
  if (activate || g_cur < 0) {
    activate_tab_(at, g_cur >= 0);
    navigate(path, false);
  } else {
    // loads in the background
    Tab *bt = g_tabs[(size_t)at].get();
    bt->cwd = path;
    bt->view->bg_trash = path == TRASH_URI;
    bt->view->load(path);
    update_tabbar();
  }
}

static void close_tab(int i) {
  if (i < 0 || i >= (int)g_tabs.size()) return;
  if (g_tabs.size() == 1) {
    g_win->hide();
    return;
  }
  if (i != g_cur) save_tab();
  Tab *t = g_tabs[(size_t)i].get();
  if (g_split && (t == g_pane[0] || t == g_pane[1])) {
    for (Tab *pt : g_pane)
      if (pt && pt != t && pt->view != g_view) pt->view->hide();
    g_split = false;
    g_pane[0] = g_pane[1] = nullptr;
  }
  t->search->cancel();
  Fl::remove_timeout(search_poll, t);
  Fl::remove_idle(FileView::thumb_idle, t->view);
  g_views->remove(t->view);
  Fl::delete_widget(t->view);
  if (g_view == t->view) g_view = nullptr;
  bool was_cur = i == g_cur;
  g_tabs.erase(g_tabs.begin() + i);
  if (was_cur) {
    g_cur = -1;
    activate_tab_(std::min(i, (int)g_tabs.size() - 1), false);
  } else {
    if (i < g_cur) g_cur--;
    activate_tab_(g_cur, false);
  }
}

static void open_dir_elsewhere(const std::string &path) {
  if (S.middle_tab) new_tab(path, false);
  else fld::spawn({"fl-files", path});
}

static void leave_folder(const std::string &dir) {
  auto inside = [&](const std::string &p) { return p == dir || fld::starts_with(p, dir + "/"); };
  int was = g_cur;
  for (int k = 0; k < (int)g_tabs.size(); k++) {
    bool cur = k == g_cur;
    if (!inside(cur ? g_cwd : g_tabs[(size_t)k]->cwd)) continue;
    if (!cur) activate_tab(k);
    if (g_searching) { g_search->cancel(); g_searching = false; }
    navigate(fld::home_dir());
  }
  if (was != g_cur) activate_tab(was);
}

static void tab_menu(int i) {
  bool on_tab = i >= 0;
  int n = (int)g_tabs.size();
  Fl_Menu_Item m[] = {{"New Tab", FL_CTRL + 't', nullptr, (void *)1},
                      {"Duplicate Tab", 0, nullptr, (void *)2, on_tab ? 0 : FL_MENU_INACTIVE},
                      {"Open in New Window", 0, nullptr, (void *)3, (on_tab ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER},
                      {"Close Other Tabs", 0, nullptr, (void *)4, on_tab && n > 1 ? 0 : FL_MENU_INACTIVE},
                      {"Close Tabs to the Right", 0, nullptr, (void *)5, on_tab && i < n - 1 ? 0 : FL_MENU_INACTIVE},
                      {"Close Tab", FL_CTRL + 'w', nullptr, (void *)6, on_tab ? 0 : FL_MENU_INACTIVE},
                      {nullptr}};
  const Fl_Menu_Item *r = m->popup(Fl::event_x(), Fl::event_y());
  if (!r) return;
  auto cwd_of = [](int k) { return k == g_cur ? g_cwd : g_tabs[(size_t)k]->cwd; };
  switch ((long)r->user_data()) {
    case 1: new_tab(in_trash() || g_cwd.empty() ? fld::home_dir() : g_cwd); break;
    case 2: activate_tab(i); new_tab(g_cwd); break;
    case 3: {
      std::string d = cwd_of(i);
      fld::spawn({"fl-files", d});
      close_tab(i);
      break;
    }
    case 4:
      activate_tab(i);
      while (g_tabs.size() > 1) close_tab(g_cur == 0 ? 1 : 0);
      break;
    case 5:
      activate_tab(i);
      while ((int)g_tabs.size() > i + 1) close_tab(i + 1);
      break;
    case 6: close_tab(i); break;
  }
}

static fld::TabBar *make_tabbar(int W) {
  fld::TabBar *tb = new fld::TabBar(0, 0, W, fld::TabBar::preferred_height());
  tb->on_select = [](int i) { activate_tab(i); };
  tb->on_close = [](int i) { close_tab(i); };
  tb->on_new = [] { new_tab(in_trash() || g_cwd.empty() ? fld::home_dir() : g_cwd); };
  tb->on_menu = [](int i) { tab_menu(i); };
  tb->on_move = [](int from, int to) {
    save_tab();
    fld::move_element(g_tabs, from, to);
    g_cur = fld::moved_index(g_cur, from, to);
    activate_tab_(g_cur, false);
  };
  tb->on_detach = [](int i) {
    std::string d = i == g_cur ? g_cwd : g_tabs[(size_t)i]->cwd;
    fld::spawn({"fl-files", d == TRASH_URI ? std::string(TRASH_URI) : d});
    close_tab(i);
  };
  // tabs move to other fl-files windows with their history
  tb->app_id = "fl-files";
  tb->on_export = [](int i) { return export_tab(i); };
  tb->on_import = [](const std::string &state, int at, bool) { return import_tab(state, at); };
  tb->on_moved_away = [](int i) { close_tab(i); };
  tb->on_tear_off = [](int i, const std::string &file) {
    if (fld::spawn({fld::self_program("fl-files"), "--adopt-tab", file})) close_tab(i);
    else fld::discard_tab_file(file); // the tab stays here
  };
  tb->hide();
  return tb;
}

// a tab's folder and history, for moving it to another window
static std::string export_tab(int i) {
  if (i < 0 || i >= (int)g_tabs.size()) return "";
  bool cur = i == g_cur;
  Tab *t = g_tabs[(size_t)i].get();
  fld::Json j = fld::Json::object();
  j.set("cwd", cur ? g_cwd : t->cwd);
  fld::Json back = fld::Json::array(), fwd = fld::Json::array();
  for (auto &b : cur ? g_hist_back : t->back) back.push(b);
  for (auto &f : cur ? g_hist_fwd : t->fwd) fwd.push(f);
  j.set("back", back);
  j.set("fwd", fwd);
  j.set("filter", cur ? std::string(g_filter->value()) : t->filter);
  return j.dump();
}

// opens a tab from export_tab() at index `at` (and shows it)
static bool import_tab(const std::string &state, int at) {
  fld::Json j;
  if (!fld::Json::parse(state, j)) return false;
  std::string cwd = j["cwd"].as_string();
  if (cwd != TRASH_URI && cwd != NETWORK_URI && !fld::is_dir(cwd)) return false;
  size_t before = g_tabs.size();
  new_tab(cwd);
  if (g_tabs.size() == before) return false;
  g_hist_back.clear();
  g_hist_fwd.clear();
  for (auto &b : j["back"].items()) g_hist_back.push_back(b.as_string());
  for (auto &f : j["fwd"].items()) g_hist_fwd.push_back(f.as_string());
  std::string filter = j["filter"].as_string();
  if (!filter.empty()) {
    g_filter->value(filter.c_str());
    g_view->apply_filter_sort();
  }
  int to = std::max(0, std::min((int)g_tabs.size() - 1, at));
  if (to != g_cur) {
    save_tab();
    int from = g_cur;
    fld::move_element(g_tabs, from, to);
    g_cur = to;
    activate_tab_(g_cur, false);
  }
  update_nav_buttons();
  update_status();
  g_win->show(); // raise it: the tab arrived here
  return true;
}

// ---------------------------------------------------------------------------
// Network locations (GVfs)

// Mounts a location (smb://server/share...) and opens its folder here, in a
// new window or in a new tab
static void mount_and_go(const std::string &uri, int where) {
#ifdef FLD_HAVE_GVFS
  if (gv::uri_scheme(uri) == "network") {
    if (where == 2) new_tab(NETWORK_URI);
    else if (where == 1) fld::spawn({"fl-files", NETWORK_URI});
    else navigate(NETWORK_URI);
    return;
  }
  if (where == 1) {
    fld::spawn({"fl-files", uri});
    return;
  }
  std::string name = gv::display_name_for_uri(uri);
  g_status->copy_label(fld::escape_at("Connecting to " + name + "…").c_str());
  gv::Client::get().mount_uri(uri, [uri, name, where](const std::string &err, const std::string &path) {
    update_status();
    if (err == gv::kCancelled) return;
    if (!err.empty()) {
      fl_alert("Could not open %s:\n%s", fld::escape_at(uri).c_str(), fld::escape_at(err).c_str());
      return;
    }
    gv::Client::get().refresh();
    if (where == 2) new_tab(path);
    else navigate(path);
  });
#else
  (void)where;
  fl_alert("fl-files was built without GVfs: it cannot open %s.", fld::escape_at(uri).c_str());
#endif
}

static void connect_to_server() {
#ifdef FLD_HAVE_GVFS
  fld::Ini c;
  c.load(settings_path());
  std::vector<std::string> recent;
  for (int i = 0; i < 10; i++) {
    std::string r = c.get("Servers", "Recent" + std::to_string(i), "");
    if (!r.empty()) recent.push_back(r);
  }
  bool bookmark = false;
  std::string uri = connect_dialog(recent, bookmark);
  if (!uri.empty() && !gv::looks_like_uri(uri)) {
    fl_alert("\"%s\" is not a server address.\nIt starts with the kind of server, for example smb://, sftp:// or ftp://.",
             fld::escape_at(uri).c_str());
    uri.clear();
  }
  if (!uri.empty()) {
    recent.erase(std::remove(recent.begin(), recent.end(), uri), recent.end());
    recent.insert(recent.begin(), uri);
  }
  c.remove_group("Servers");
  for (size_t i = 0; i < recent.size() && i < 10; i++) c.set("Servers", "Recent" + std::to_string(i), recent[i]);
  c.save(settings_path());
  if (uri.empty()) return;
  if (bookmark) add_bookmark_uri(uri, gv::display_name_for_uri(uri));
  mount_and_go(uri);
#else
  fl_alert("fl-files was built without GVfs support.");
#endif
}

static void edit_location();
static void toggle_menubar();
static void select_all();

static int global_keys(int e) {
  if (e != FL_SHORTCUT) return 0;
  int k = Fl::event_key();
  int st = Fl::event_state();
  bool ctrl = st & FL_CTRL, alt = st & FL_ALT, shift = st & FL_SHIFT;
  if (alt && k == FL_Left) { go_back(); return 1; }
  if (alt && k == FL_Right) { go_forward(); return 1; }
  if (alt && k == FL_Up) { go_up(); return 1; }
  if (alt && k == FL_Home) { navigate(fld::home_dir()); return 1; }
  if (k == FL_F + 9) { show_tree(!S.show_tree); return 1; }
  if (k == FL_F + 4) { toggle_split(); return 1; }
  if (k == FL_F + 3 || (ctrl && shift && (k == 'f' || k == 'F'))) { show_search_bar(true); return 1; }
  if (k == FL_Escape && in_search() && Fl::focus() == g_view) { show_search_bar(false); return 1; }
  if (k == FL_BackSpace && Fl::focus() == g_view) { go_up(); return 1; }
  if (k == FL_F + 5) { refresh(); return 1; }
  // tabs: Alt+1..9, Ctrl+Tab, Ctrl+Page Up/Down
  if (alt && !ctrl && k >= '1' && k <= '9') {
    int i = k == '9' ? (int)g_tabs.size() - 1 : k - '1';
    if (i < (int)g_tabs.size()) activate_tab(i);
    return 1;
  }
  if (ctrl && (k == FL_Tab || k == FL_Page_Up || k == FL_Page_Down) && g_tabs.size() > 1) {
    int n = (int)g_tabs.size(), d = (k == FL_Page_Up || (k == FL_Tab && shift)) ? -1 : 1;
    activate_tab((g_cur + d + n) % n);
    return 1;
  }
  if (!ctrl) return 0;
  switch (k) {
    case 'l': edit_location(); return 1;
    case 'm': toggle_menubar(); return 1;
    case 'f': g_filter->take_focus(); return 1;
    case 'h': toggle_hidden(); return 1;
    case 'd': if (!in_search()) add_bookmark(g_cwd); return 1;
    case ',': show_preferences(); return 1;
    case 'n':
      if (shift) new_item(true);
      else fld::spawn({"fl-files", in_trash() ? fld::home_dir() : g_cwd});
      return 1;
    case 't': new_tab(in_trash() || in_search() ? fld::home_dir() : g_cwd); return 1;
    case 'w': close_tab(g_cur); return 1;
    case 'q': g_win->hide(); return 1;
    case '1': set_view(VIEW_ICONS); return 1;
    case '2': set_view(VIEW_COMPACT); return 1;
    case '3': set_view(VIEW_DETAILS); return 1;
    case '=': case '+': zoom(1); return 1;
    case '-': zoom(-1); return 1;
    case '0': zoom(0); return 1;
  }
  if (Fl::focus() != g_view) return 0;
  switch (k) {
    case 'a': select_all(); return 1;
    case 'c': copy_to_clipboard(false); return 1;
    case 'x': copy_to_clipboard(true); return 1;
    case 'v': if (!in_search()) paste_into(g_cwd); return 1;
    case 'z': undo(); return 1;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Menus: the toolbar's menu button and the menu bar (View > Show Menu Bar)

static void toggle_menubar() {
  S.menubar = !S.menubar;
  save_settings();
  relayout();
}

static void select_all() {
  for (int i = 0; i < (int)g_view->items.size(); i++) g_view->sel.insert(i);
  g_view->redraw();
  update_status();
}

static void edit_location() {
  g_path->value(in_trash() ? "trash:///" : g_cwd.c_str());
  g_path->take_focus();
  g_path->insert_position(0, g_path->size());
}

static std::string new_tab_dir() { return in_trash() || in_search() || g_cwd.empty() ? fld::home_dir() : g_cwd; }

static void view_items(fld::Menu &m) {
  m.radio("&Icons", V.view == VIEW_ICONS, [] { set_view(VIEW_ICONS); });
  m.radio("&Compact", V.view == VIEW_COMPACT, [] { set_view(VIEW_COMPACT); });
  m.radio("&Details", V.view == VIEW_DETAILS, [] { set_view(VIEW_DETAILS); }, FL_MENU_DIVIDER);
  m.item("Zoom &In", [] { zoom(1); }, V.zoom >= kZoomMax ? FL_MENU_INACTIVE : 0, FL_CTRL + '=');
  m.item("Zoom &Out", [] { zoom(-1); }, V.zoom <= 0 ? FL_MENU_INACTIVE : 0, FL_CTRL + '-');
  m.item("&Normal Size", [] { zoom(0); }, FL_MENU_DIVIDER, FL_CTRL + '0');
  m.item("C&olumns…", [] { columns_menu(Fl::event_x(), Fl::event_y()); });
}

// for_bar: the menu bar's menus, else the menu button's menu
static void build_menu(fld::Menu &m, bool for_bar) {
  // the trash and the network view take no new files
  bool trash = in_trash() || in_network(), search = in_search(), have_sel = g_view && !g_view->sel.empty();
  auto toggles = [&] {
    m.toggle("Folder &Tree", S.show_tree, [] { show_tree(!S.show_tree); });
    m.toggle("Show &Hidden Files", S.show_hidden, [] { toggle_hidden(); });
    m.toggle("Show T&humbnails", S.thumbnails, [] {
      S.thumbnails = !S.thumbnails;
      save_settings();
      g_view->clear_thumbs();
      refresh();
    });
    m.toggle("Show &Menu Bar", S.menubar, [] { toggle_menubar(); });
    m.toggle("S&plit View (F4)", g_split, [] { toggle_split(); });
  };
  if (!for_bar) {
    m.item("New &Tab", [] { new_tab(new_tab_dir()); }, 0, FL_CTRL + 't');
    m.item("New &Window", [] { fld::spawn({"fl-files", new_tab_dir()}); }, 0, FL_CTRL + 'n');
    m.item("New &Folder…", [] { new_item(true); }, trash || search ? FL_MENU_INACTIVE : 0, FL_CTRL + FL_SHIFT + 'n');
    m.divider();
    m.item("&Find Files…", [] { show_search_bar(true); }, 0, FL_CTRL + FL_SHIFT + 'f');
    m.item("&Bookmark This Folder", [] { add_bookmark(g_cwd); }, trash || search ? FL_MENU_INACTIVE : 0, FL_CTRL + 'd');
#ifdef FLD_HAVE_GVFS
    m.item("Connect to &Server…", [] { connect_to_server(); });
#endif
    m.divider();
    m.submenu("&View");
    view_items(m);
    m.end_submenu();
    toggles();
    m.divider();
    m.item("Open Terminal &Here", [] { fld::spawn(fld::shell_split(fld::terminal_command()), g_cwd); },
           trash || search ? FL_MENU_INACTIVE : 0);
    m.divider();
    m.item("&Preferences…", [] { show_preferences(); }, 0, FL_CTRL + ',');
    m.item("&About", [] { fld::about_dialog("Files", "fl-files", "A file manager built with FLTK."); });
    m.item("&Quit", [] { g_win->hide(); }, 0, FL_CTRL + 'q');
    return;
  }
  m.submenu("&File");
  m.item("New &Tab", [] { new_tab(new_tab_dir()); }, 0, FL_CTRL + 't');
  m.item("New &Window", [] { fld::spawn({"fl-files", new_tab_dir()}); }, 0, FL_CTRL + 'n');
  m.item("New &Folder…", [] { new_item(true); }, trash || search ? FL_MENU_INACTIVE : 0, FL_CTRL + FL_SHIFT + 'n');
  m.item("New &Empty File…", [] { new_item(false); }, trash || search ? FL_MENU_INACTIVE : 0);
  m.divider();
#ifdef FLD_HAVE_GVFS
  m.item("Connect to &Server…", [] { connect_to_server(); }, FL_MENU_DIVIDER);
#endif
  m.item("Open Terminal &Here", [] { fld::spawn(fld::shell_split(fld::terminal_command()), g_cwd); },
         (trash || search ? FL_MENU_INACTIVE : 0) | FL_MENU_DIVIDER);
  m.item("P&roperties", [] { properties(g_view->sel.empty() ? std::vector<std::string>{g_cwd} : g_view->selected_paths()); },
         trash ? FL_MENU_INACTIVE : 0, FL_ALT + FL_Enter);
  m.divider();
  m.item("&Close Tab", [] { close_tab(g_cur); }, 0, FL_CTRL + 'w');
  m.item("&Quit", [] { g_win->hide(); }, 0, FL_CTRL + 'q');
  m.end_submenu();

  m.submenu("&Edit");
  m.item(undo_label(), [] { undo(); }, (g_undo.empty() ? FL_MENU_INACTIVE : 0) | FL_MENU_DIVIDER, FL_CTRL + 'z');
  m.item("Cu&t", [] { copy_to_clipboard(true); }, have_sel && !trash ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'x');
  m.item("&Copy", [] { copy_to_clipboard(false); }, have_sel && !trash ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'c');
  m.item("&Paste", [] { paste_into(g_cwd); }, trash || search ? FL_MENU_INACTIVE : 0, FL_CTRL + 'v');
  m.item("Select &All", [] { select_all(); }, FL_MENU_DIVIDER, FL_CTRL + 'a');
  m.item("&Rename…", [] {
    if (g_view->sel.size() > 1 && !in_search()) batch_rename(g_view->selected_paths());
    else if (g_view->focus >= 0) g_view->start_rename(g_view->focus);
  },
         have_sel && !trash ? 0 : FL_MENU_INACTIVE, FL_F + 2);
  m.item("Move to &Trash", [] { trash_selected(false); }, have_sel && !trash ? 0 : FL_MENU_INACTIVE, FL_Delete);
  m.item("&Delete Permanently", [] { trash_selected(true); }, (have_sel ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER,
         FL_SHIFT + FL_Delete);
  m.item("&Find Files…", [] { show_search_bar(true); }, 0, FL_CTRL + FL_SHIFT + 'f');
  m.item("Fi&lter…", [] { g_filter->take_focus(); }, FL_MENU_DIVIDER, FL_CTRL + 'f');
  m.item("Pr&eferences…", [] { show_preferences(); }, 0, FL_CTRL + ',');
  m.end_submenu();

  m.submenu("&View");
  view_items(m);
  m.divider();
  toggles();
  m.divider();
  m.item("&Reload", [] { refresh(); }, 0, FL_F + 5);
  m.end_submenu();

  m.submenu("&Go");
  m.item("&Back", [] { go_back(); }, g_back->active() ? 0 : FL_MENU_INACTIVE, FL_ALT + FL_Left);
  m.item("&Forward", [] { go_forward(); }, g_fwd->active() ? 0 : FL_MENU_INACTIVE, FL_ALT + FL_Right);
  m.item("&Up", [] { go_up(); }, g_up->active() ? 0 : FL_MENU_INACTIVE, FL_ALT + FL_Up);
  m.divider();
  m.item("&Home", [] { navigate(fld::home_dir()); }, 0, FL_ALT + FL_Home);
  m.item("&Trash", [] { navigate(TRASH_URI); });
#ifdef FLD_HAVE_GVFS
  m.item("&Network", [] { navigate(NETWORK_URI); });
#endif
  m.item("File &System", [] { navigate("/"); }, FL_MENU_DIVIDER);
  m.item("&Location…", [] { edit_location(); }, 0, FL_CTRL + 'l');
  m.end_submenu();

  m.submenu("&Bookmarks");
  m.item("&Bookmark This Folder", [] { add_bookmark(g_cwd); }, trash || search ? FL_MENU_INACTIVE : 0, FL_CTRL + 'd');
  bool first = true;
  for (auto &p : g_place_list) {
    if (!p.bookmark) continue;
    if (first) m.divider();
    first = false;
    std::string path = p.path;
    m.item(fld::Menu::text(p.label), [path] { navigate(path); });
  }
  m.end_submenu();

  m.submenu("&Help");
  m.item("&About Files", [] { fld::about_dialog("Files", "fl-files", "A file manager built with FLTK."); });
  m.end_submenu();
}

// The menu bar is built again each time it opens (selection, toggles, bookmarks)
class FilesMenuBar : public fld::MenuBar {
public:
  fld::Menu menu;
  // rebuild() fills it, once the window is built
  using fld::MenuBar::MenuBar;
  void rebuild() {
    menu = fld::Menu();
    build_menu(menu, true);
    const Fl_Menu_Item *mi = menu.items();
    items_.assign(mi, mi + count_items(mi));
    for (auto &it : items_)
      if (it.text && !(it.flags & FL_SUBMENU)) it.callback_ = cb;
    Fl_Menu_Bar::menu(items_.data());
  }
  int handle(int e) override {
    if (e == FL_PUSH) rebuild();
    // the shortcuts are the window's (they also work with the bar hidden)
    if (e == FL_SHORTCUT) return 0;
    return fld::MenuBar::handle(e);
  }

private:
  std::vector<Fl_Menu_Item> items_;
  static int count_items(const Fl_Menu_Item *m) {
    int n = 0, depth = 0;
    for (;; n++) {
      if (!m[n].text) {
        if (depth == 0) break;
        depth--;
        continue;
      }
      if (m[n].flags & FL_SUBMENU) depth++;
    }
    return n + 1;
  }
  static void cb(Fl_Widget *w, void *d) { ((FilesMenuBar *)w)->menu.run((long)d); }
};

static void rebuild_menubar() { ((FilesMenuBar *)g_menubar)->rebuild(); }

class FilesWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  int handle(int e) override {
    // mouse back/forward buttons
    if (e == FL_PUSH && (Fl::event_button() == 8 || Fl::event_button() == 9)) {
      if (Fl::event_button() == 8) go_back(); else go_forward();
      return 1;
    }
    return Fl_Double_Window::handle(e);
  }
};

int main(int argc, char **argv) {
  fld::init_app("fl-files", argc, argv);
  Fl::lock(); // enable Fl::awake from worker threads
  load_settings();
  std::string start = fld::home_dir(), start_uri, adopt_file;
  bool connect = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-files [--connect] [folder|trash:///|network:///|smb://server/share|sftp://host/...]\n");
      return 0;
    }
    if (a == "--connect") { connect = true; continue; }
    if (a == "--adopt-tab" && i + 1 < argc) { adopt_file = argv[++i]; continue; }
    if (a[0] == '-') continue;
    if (a == NETWORK_URI || a == "network:") { start = NETWORK_URI; continue; }
#ifdef FLD_HAVE_GVFS
    if (gv::looks_like_uri(a)) { start_uri = a; continue; }
#endif
    if (a == TRASH_URI || a == "trash:") start = TRASH_URI;
    else if (fld::starts_with(a, "file://")) start = fld::uri_decode(a.substr(7));
    else start = a;
    if (start != TRASH_URI && start != NETWORK_URI && !fld::is_dir(start)) start = fld::dirname(fld::abs_path(start));
  }

  const int W = 980, H = 640, TB = TOOLBAR_H, ST = STATUS_H;
  const fld::Palette &p = fld::palette();
  g_win = new FilesWindow(W, H, "Files");
  // the close button and Ctrl+Q close the window; Escape (FLTK's default) does not
  g_win->callback([](Fl_Widget *w, void *) {
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
    w->hide();
  });

  g_menubar = new FilesMenuBar(0, 0, W, MENUBAR_H);
  g_menubar->box(FL_FLAT_BOX);
  g_menubar->hide();
  fld::Toolbar *bar = g_toolbar = new fld::Toolbar(0, 0, W, TB);
  bar->color(p.bg);
  g_back = bar->add_button("go-previous", "Back (Alt+Left)", [](Fl_Widget *, void *) { go_back(); }, nullptr);
  g_fwd = bar->add_button("go-next", "Forward (Alt+Right)", [](Fl_Widget *, void *) { go_forward(); }, nullptr);
  g_up = bar->add_button("go-up", "Parent folder (Alt+Up)", [](Fl_Widget *, void *) { go_up(); }, nullptr);
  bar->add_button("go-home", "Home (Alt+Home)", [](Fl_Widget *, void *) { navigate(fld::home_dir()); }, nullptr);
  bar->add_separator();
  int px = bar->next_x();
  g_path = new Fl_Input(px, 7, W - px - 280, 28);
  g_path->when(FL_WHEN_ENTER_KEY_ALWAYS);
  g_path->callback([](Fl_Widget *w, void *) {
    std::string v = fld::trim(((Fl_Input *)w)->value());
    if (v == "Trash" || v == "trash:///" || v == "trash:") navigate(TRASH_URI);
    else if (v == "Network" || v == "network:///" || v == "network:") navigate(NETWORK_URI);
#ifdef FLD_HAVE_GVFS
    else if (gv::looks_like_uri(v)) mount_and_go(v);
#endif
    else if (fld::starts_with(v, "file://")) navigate(fld::uri_decode(v.substr(7)));
    else navigate(fld::expand_tilde(v));
  });
  g_path->tooltip("Location (Ctrl+L)");
  bar->add(g_path);
  g_filter = new fld::SearchInput(W - 270, 7, 112, 28, "Filter");
  g_filter->when(FL_WHEN_CHANGED);
  g_filter->callback([](Fl_Widget *, void *) { g_view->apply_filter_sort(); update_status(); });
  g_filter->tooltip("Filter by name (Ctrl+F)");
  bar->add(g_filter);
  fld::FlatButton *find_btn = new fld::FlatButton(W - 154, 3, 36, 36);
  find_btn->color(p.bg);
  find_btn->icon_name("system-search", 20);
  if (!fld::icon("system-search", 20)) find_btn->icon_name("edit-find", 20);
  find_btn->tooltip("Find files and folders in this folder (Ctrl+Shift+F)");
  find_btn->callback([](Fl_Widget *, void *) { show_search_bar(!g_sbar->visible()); });
  bar->add(find_btn);
  fld::FlatButton *refresh_btn = new fld::FlatButton(W - 114, 3, 36, 36);
  refresh_btn->color(p.bg);
  refresh_btn->icon_name("view-refresh", 20);
  refresh_btn->tooltip("Reload (F5)");
  refresh_btn->callback([](Fl_Widget *, void *) { refresh(); });
  bar->add(refresh_btn);
  g_view_btn = new fld::FlatButton(W - 76, 3, 36, 36);
  g_view_btn->color(p.bg);
  g_view_btn->tooltip("Switch between icons and details (Ctrl+1 icons, Ctrl+2 compact, Ctrl+3 details)");
  g_view_btn->callback([](Fl_Widget *, void *) { set_view(V.view == VIEW_DETAILS ? VIEW_ICONS : VIEW_DETAILS); });
  bar->add(g_view_btn);
  fld::FlatButton *menu_btn = new fld::FlatButton(W - 40, 3, 36, 36);
  menu_btn->color(p.bg);
  menu_btn->icon_name("open-menu", 20);
  if (!fld::icon("open-menu", 20)) menu_btn->icon_name("preferences-system", 20);
  menu_btn->tooltip("Menu");
  menu_btn->callback([](Fl_Widget *, void *) {
    fld::Menu m;
    build_menu(m, false);
    m.popup();
  });
  bar->add(menu_btn);
  bar->resizable(g_path);
  bar->end();

  g_tabbar = make_tabbar(W);
  g_win->add(g_tabbar);
  Fl_Group::current(g_win);
  build_search_bar(W, TB);
  Fl_Tile *tile = g_tile = new Fl_Tile(0, TB, W, H - TB - ST);
  Fl_Group *side = new Fl_Group(0, TB, S.sidebar_w, H - TB - ST);
  g_tree = new DropTree(0, TB, S.sidebar_w, H - TB - ST - 40);
  g_tree->showroot(0);
  g_tree->box(FL_FLAT_BOX);
  g_tree->color(p.bg);
  g_tree->item_labelsize(13);
  g_tree->item_labelfgcolor(p.fg);
  g_tree->selection_color(p.accent);
  g_tree->connectorstyle(FL_TREE_CONNECTOR_NONE);
  g_tree->selectmode(FL_TREE_SELECT_SINGLE);
  g_tree->callback(tree_cb);
  g_tree->when(FL_WHEN_CHANGED);
  g_tree->clear_visible_focus();
  g_tree->hide();
  g_places = new PlacesBrowser(0, TB, S.sidebar_w, H - TB - ST - 40);
  g_places->box(FL_FLAT_BOX);
  g_places->color(p.bg);
  g_places->textsize(13);
  g_places->selection_color(p.accent);
  g_places->callback([](Fl_Widget *w, void *) {
    int line = ((Fl_Hold_Browser *)w)->value();
    if (line > 0 && line <= (int)g_place_list.size()) open_place(g_place_list[line - 1]);
  });
  g_places->clear_visible_focus();
  g_empty_trash = new fld::FlatButton(6, H - ST - 38, S.sidebar_w - 12, 32, "Empty Trash");
  g_empty_trash->color(p.bg);
  g_empty_trash->icon_name("user-trash", 18);
  g_empty_trash->callback([](Fl_Widget *, void *) {
    if (fl_choice("Permanently delete all items in the Trash?", "Cancel", "Empty Trash", nullptr) == 1) {
      fld::trash_empty();
      refresh();
    }
  });
  Fl_Box *side_fill = new Fl_Box(0, H - ST - 40, S.sidebar_w, 40);
  side_fill->box(FL_FLAT_BOX);
  side_fill->color(p.bg);
  side->resizable(g_places);
  g_tree->resize(g_places->x(), g_places->y(), g_places->w(), g_places->h());
  side->end();
  g_empty_trash->parent()->remove(g_empty_trash);
  side->add(g_empty_trash);
  g_views = new Fl_Group(S.sidebar_w, TB, W - S.sidebar_w, H - TB - ST);
  g_views->end(); // the tabs' views are added by new_tab()
  tile->end();
  tile->resizable(g_views);
  tile->size_range(side, 120, 120, 400, 10000);
  // status bar: text, then the zoom slider
  const int ZW = 170;
  Fl_Group *status_bar = new Fl_Group(0, H - ST, W, ST);
  g_status = new Fl_Box(0, H - ST, W - ZW, ST);
  g_status->box(FL_FLAT_BOX);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(p.fg_dim);
  Fl_Box *zbg = new Fl_Box(W - ZW, H - ST, ZW, ST);
  zbg->box(FL_FLAT_BOX);
  fld::FlatButton *zout = new fld::FlatButton(W - ZW + 4, H - ST + 1, 22, ST - 2, "\u2212");
  zout->tooltip("Zoom Out (Ctrl+-)");
  zout->callback([](Fl_Widget *, void *) { zoom(-1); });
  g_zoom_slider = new Fl_Slider(W - ZW + 30, H - ST + 6, ZW - 60, ST - 12);
  g_zoom_slider->type(FL_HOR_NICE_SLIDER);
  g_zoom_slider->box(FL_FLAT_BOX);
  g_zoom_slider->color(fl_color_average(p.border, p.bg, 0.5f));
  g_zoom_slider->selection_color(p.accent);
  g_zoom_slider->bounds(0, kZoomMax);
  g_zoom_slider->step(1);
  g_zoom_slider->clear_visible_focus();
  g_zoom_slider->callback([](Fl_Widget *w, void *) { set_zoom((int)((Fl_Slider *)w)->value()); });
  fld::FlatButton *zin = new fld::FlatButton(W - 26, H - ST + 1, 22, ST - 2, "+");
  zin->tooltip("Zoom In (Ctrl+=)");
  zin->callback([](Fl_Widget *, void *) { zoom(1); });
  status_bar->resizable(g_status);
  status_bar->end();
  update_zoom_slider();
  g_win->resizable(tile);
  g_win->end();
  g_win->size_range(500, 320);
  fld::set_window_icon(g_win, "fl-files");
  Fl::add_handler(global_keys);

  g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (g_inotify >= 0) Fl::add_fd(g_inotify, FL_READ, inotify_cb);

#ifdef FLD_HAVE_UDISKS
  ud::Client::get().on_changed = [] {
    fill_places();
    select_place_for(g_cwd);
  };
  ud::Client::get().start();
#endif
#ifdef FLD_HAVE_GVFS
  gv::Client &gc = gv::Client::get();
  gc.ask_password = gvfs_password_dialog;
  gc.ask_question = gvfs_question_dialog;
  gc.on_changed = [] {
    fill_places();
    select_place_for(g_cwd);
    show_location(); // a mount's name and location
    update_tabbar();
  };
  gc.start();
#endif
  fill_places();
  update_view_button();
  // a tab dragged out of another window: its state, else the folder asked for
  std::string adopted;
  if (!adopt_file.empty() && fld::claim_tab_file(adopt_file, "fl-files", adopted) && import_tab(adopted, 0)) {
  } else new_tab(start);
  if (g_tabs.empty()) new_tab(fld::home_dir()); // the folder could not be opened
  if (S.show_tree) show_tree(true);
  g_win->show(); // not show(argc, argv): it resets the theme colors
  g_view->take_focus();
  if (!start_uri.empty()) mount_and_go(start_uri);
  if (connect) Fl::add_timeout(0.2, [](void *) { connect_to_server(); });
  return Fl::run();
}
