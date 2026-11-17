// fl-files: file manager for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Menu_Button.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Tree.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/fl_utf8.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <deque>
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
#include <vector>

#include "fileops.h"
#include "search.h"
#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/mime.h"
#include "fld/theme.h"
#include "fld/trash.h"
#include "fld/util.h"
#include "fld/widgets.h"

static const char *TRASH_URI = "trash:///";

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

// How a folder is shown: per folder when remembered, else the defaults
struct ViewState {
  ViewMode view = VIEW_ICONS;
  SortKey sort = SORT_NAME;
  bool desc = false;
  int zoom = 2;
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
} S;

static std::string settings_path() { return fld::fld_config_dir() + "/files.ini"; }
static std::string folder_settings_path() { return fld::fld_config_dir() + "/files-folders.ini"; }

static void save_settings() {
  fld::Ini c;
  c.load(settings_path());
  c.set_bool("Files", "ShowHidden", S.show_hidden);
  c.set_int("Files", "View", S.def.view);
  c.remove("Files", "ListView");
  c.set_int("Files", "Zoom", S.def.zoom);
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
  S.def.zoom = std::max(0, std::min(6, c.get_int("Files", "Zoom", 2)));
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
  v.zoom = std::max(0, std::min(6, c.get_int(dir, "Zoom", v.zoom)));
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
  c.set_int(dir, "Zoom", V.zoom);
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

static search::Search g_search;
static std::string g_search_desc; // e.g. "*.txt" for the title and status
static bool g_search_running() { return g_search.running(); }

static void navigate(const std::string &path, bool record = true);
static void fill_places();
static void select_place_for(const std::string &path);
static void refresh();
static void update_status();
static void open_entry(Entry &e);
static void run_op(FileOp::Kind k, const std::vector<std::string> &src, const std::string &dest);

static bool in_trash() { return g_cwd == TRASH_URI && !g_searching; }
static bool in_search() { return g_searching; }
static bool g_search_running();

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
  int press_x = 0, press_y = 0;
  int resizing_col = -1, resize_x0 = 0, resize_w0 = 0;
  std::string type_ahead;

  static const int HEADER_H = 26, COMPACT_W = 240;

  FileView(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
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
  ~FileView() override { clear_thumbs(); }

  void clear_thumbs() {
    for (auto &e : all) { delete e.thumb; e.thumb = nullptr; e.thumb_tried = false; }
  }
  // after a zoom change: icons and thumbnails are size dependent
  void clear_images() {
    clear_thumbs();
    for (auto &e : all) e.icon_big = e.icon_small = nullptr;
  }

  // ---- geometry
  static int big() { static const int s[] = {24, 32, 48, 64, 80, 96, 128}; return s[V.zoom]; }
  static int small() { static const int s[] = {16, 16, 20, 24, 24, 32, 32}; return s[V.zoom]; }
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
    std::string f = fld::lower(g_filter ? g_filter->value() : "");
    items.clear();
    for (int i = 0; i < (int)all.size(); i++) {
      const Entry &e = all[i];
      if (!S.show_hidden && !in_trash() && !in_search() && e.name[0] == '.') continue;
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
        case SORT_LOCATION: r = strcasecmp(fld::dirname(x.path).c_str(), fld::dirname(y.path).c_str()); break;
        default: break;
      }
      if (r == 0) r = strcasecmp(x.name.c_str(), y.name.c_str());
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
    if (dir == TRASH_URI) {
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

  std::vector<std::string> wrap_name(const std::string &s, int width, int max_lines) {
    std::vector<std::string> lines;
    std::string cur;
    size_t i = 0;
    while (i < s.size()) {
      int len = std::max(1, fl_utf8len1(s[i]));
      std::string ch = s.substr(i, len);
      if (fl_width((cur + ch).c_str()) > width && !cur.empty()) {
        lines.push_back(cur);
        cur.clear();
        if ((int)lines.size() == max_lines) break;
      }
      cur += ch;
      i += len;
    }
    if ((int)lines.size() < max_lines) {
      if (!cur.empty()) lines.push_back(cur);
    } else if (i < s.size()) {
      std::string &l = lines.back();
      while (!l.empty() && fl_width((l + "…").c_str()) > width) {
        l.pop_back();
        while (!l.empty() && (l.back() & 0xC0) == 0x80) l.pop_back();
      }
      l += "…";
    }
    return lines;
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
    const fld::Palette &p = fld::palette();
    fl_push_clip(x(), y(), w(), h());
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    if (details()) draw_header();
    fl_push_clip(x(), view_top(), content_w(), view_h());
    bool focused = Fl::focus() == this;
    bool any_missing_thumb = false;
    if (items.empty()) {
      fl_font(FL_HELVETICA, 14);
      fl_color(p.fg_dim);
      const char *msg = in_search() ? (g_search_running() ? "Searching…" : "No files found")
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
      bool s = sel.count(i) > 0;
      bool cut = g_clip.cut && std::find(g_clip.paths.begin(), g_clip.paths.end(), e.path) != g_clip.paths.end();
      if (V.view == VIEW_DETAILS) {
        if (s) {
          fl_color(focused ? p.selection : fl_color_average(p.selection, p.bg2, 0.4f));
          fl_rectf(ix, iy, iw, ih);
        } else if (i % 2) {
          fl_color(fl_color_average(p.bg2, p.bg, 0.6f));
          fl_rectf(ix, iy, iw, ih);
        }
        Fl_Color fg = s ? p.accent_fg : p.fg;
        if (Fl_Image *ic = icon_for(e, false)) ic->draw(ix + 8, iy + (ih - SM) / 2);
        fl_font(FL_HELVETICA, 13);
        fl_color(cut ? p.fg_dim : fg);
        int cx = ix + 8;
        int nw = name_col_w();
        fl_draw(fld::escape_at(e.name).c_str(), cx + SM + 8, iy, nw - SM - 16, ih, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
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
        if (s) {
          fl_color(focused ? p.selection : fl_color_average(p.selection, p.bg2, 0.4f));
          fl_rounded_rectf(ix, iy, iw - 6, ih, 4);
        }
        if (Fl_Image *ic = icon_for(e, false)) ic->draw(ix + 4, iy + (ih - SM) / 2);
        fl_font(FL_HELVETICA, 13);
        fl_color(s ? p.accent_fg : cut ? p.fg_dim : p.fg);
        fl_draw(fld::escape_at(e.name).c_str(), ix + SM + 10, iy, iw - SM - 20, ih, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      } else {
        if (s) {
          fl_color(focused ? fl_color_average(p.selection, p.bg2, 0.35f) : fl_color_average(p.selection, p.bg2, 0.18f));
          fl_rounded_rectf(ix + 4, iy, iw - 8, ih - 4, 8);
        }
        Fl_Image *ic = e.thumb ? e.thumb : icon_for(e, true);
        if (!e.thumb_tried && S.thumbnails && !e.is_dir) any_missing_thumb = true;
        if (ic) {
          int icx = ix + (iw - ic->w()) / 2, icy = iy + 4 + (B - ic->h()) / 2;
          ic->draw(icx, icy);
          if (e.thumb) {
            fl_color(p.border);
            fl_rect(icx - 1, icy - 1, ic->w() + 2, ic->h() + 2);
          }
        }
        if (e.is_link) {
          if (Fl_Image *em = fld::icon("emblem-symbolic-link", 16)) em->draw(ix + iw / 2 + B / 4, iy + B - 12);
        }
        fl_font(FL_HELVETICA, 12);
        fl_color(cut ? p.fg_dim : p.fg);
        auto lines = wrap_name(e.name, iw - 12, s ? 3 : 2);
        int ly = iy + B + 8;
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
      fl_color(p.accent);
      fl_rect(std::min(bx0, bx1), std::min(by0, by1) - top, std::abs(bx1 - bx0), std::abs(by1 - by0));
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
};

// ---------------------------------------------------------------------------
// Operations

static void show_errors(FileOp *op) {
  if (op->errors.empty()) return;
  std::string m;
  for (size_t i = 0; i < op->errors.size() && i < 8; i++) m += op->errors[i] + "\n";
  if (op->errors.size() > 8) m += "…and " + std::to_string(op->errors.size() - 8) + " more";
  fl_alert("%s", m.c_str());
}

struct ProgressWin {
  Fl_Double_Window *win;
  Fl_Progress *bar;
  Fl_Box *label;
  FileOp *op;
};

static void progress_tick(void *d) {
  ProgressWin *p = (ProgressWin *)d;
  if (p->op->finished) return;
  double frac = p->op->total_bytes ? (double)p->op->done_bytes / p->op->total_bytes
                                   : (double)p->op->done_items / std::max<size_t>(1, p->op->sources.size());
  p->bar->value((float)(frac * 100));
  {
    std::lock_guard<std::mutex> l(p->op->mtx);
    std::string t = p->op->current;
    if (p->op->total_bytes)
      t += "   " + fld::human_size(p->op->done_bytes) + " of " + fld::human_size(p->op->total_bytes);
    p->label->copy_label(fld::escape_at(t).c_str());
  }
  if (!p->win->shown() && p->op->total_bytes > (8u << 20)) p->win->show();
  Fl::repeat_timeout(0.2, progress_tick, d);
}

static void run_op(FileOp::Kind k, const std::vector<std::string> &src, const std::string &dest) {
  if (src.empty()) return;
  FileOp *op = new FileOp;
  op->kind = k;
  op->sources = src;
  op->dest_dir = dest;
  if (k == FileOp::COPY || k == FileOp::MOVE) {
    int conflicts = 0;
    for (auto &s : src) {
      std::string d = fld::path_join(dest, fld::basename(s));
      if (fld::file_exists(d) && fld::abs_path(d) != fld::abs_path(s)) conflicts++;
    }
    if (conflicts) {
      int c = fl_choice("%d item%s already exist%s in the destination.", "Cancel", "Keep Both", "Replace",
                        conflicts, conflicts == 1 ? "" : "s", conflicts == 1 ? "s" : "");
      if (c == 0) { delete op; return; }
      op->conflict = c == 1 ? FileOp::KEEP_BOTH : FileOp::REPLACE;
    }
  }
  ProgressWin *pw = new ProgressWin;
  pw->op = op;
  const char *title = k == FileOp::COPY ? "Copying" : k == FileOp::MOVE ? "Moving" : k == FileOp::TRASH ? "Moving to Trash" : "Deleting";
  pw->win = new Fl_Double_Window(420, 110, title);
  pw->label = new Fl_Box(12, 10, 396, 24);
  pw->label->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  pw->bar = new Fl_Progress(12, 40, 396, 20);
  pw->bar->minimum(0);
  pw->bar->maximum(100);
  pw->bar->selection_color(fld::palette().accent);
  Fl_Button *cancel = new Fl_Button(308, 70, 100, 30, "Cancel");
  cancel->callback([](Fl_Widget *, void *d) { ((FileOp *)d)->cancel = true; }, op);
  pw->win->end();
  pw->win->set_non_modal();
  Fl::add_timeout(0.3, progress_tick, pw);
  op->start([pw](FileOp *o) {
    Fl::remove_timeout(progress_tick, pw);
    pw->win->hide();
    Fl::delete_widget(pw->win);
    show_errors(o);
    if (g_clip.cut && o->kind == FileOp::MOVE) { g_clip.paths.clear(); g_clip.cut = false; }
    delete pw;
    // the FileOp deletes itself after its worker has been joined
    Fl::add_timeout(0, [](void *d) { delete (FileOp *)d; }, o);
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
  if (in_trash() || dir == TRASH_URI || (in_search() && dir == g_cwd)) return;
  if (g_clip.paths.empty()) {
    Fl::paste(*g_view, 1); // maybe file URIs from another program
    return;
  }
  run_op(g_clip.cut ? FileOp::MOVE : FileOp::COPY, g_clip.paths, dir);
}

static void trash_selected(bool permanent) {
  auto paths = g_view->selected_paths();
  if (paths.empty()) return;
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
  if (in_trash() || in_search()) return;
  const char *n = fl_input(folder ? "Folder name:" : "File name:", folder ? "New Folder" : "New File");
  if (!n || !*n) return;
  if (strchr(n, '/')) { fl_alert("Names cannot contain '/'."); return; }
  std::string p = fld::path_join(g_cwd, n);
  if (fld::file_exists(p)) { fl_alert("\"%s\" already exists.", n); return; }
  bool ok = folder ? mkdir(p.c_str(), 0755) == 0 : fld::write_file(p, "");
  if (!ok) { fl_alert("Could not create \"%s\": %s", n, strerror(errno)); return; }
  g_view->load(g_cwd, n);
}

void FileView::start_rename(int i) {
  if (i < 0 || in_trash()) return;
  Entry &e = entry(i);
  const char *n = fl_input("Rename to:", e.name.c_str());
  if (!n || !*n || e.name == n) return;
  if (strchr(n, '/')) { fl_alert("Names cannot contain '/'."); return; }
  std::string dst = fld::path_join(fld::dirname(e.path), n);
  if (fld::file_exists(dst) && strcasecmp(n, e.name.c_str()) != 0) { fl_alert("\"%s\" already exists.", n); return; }
  if (rename(e.path.c_str(), dst.c_str()) != 0) { fl_alert("Rename failed: %s", strerror(errno)); return; }
  if (in_search()) {
    e.path = dst;
    e.name = n;
    restat_all();
    return;
  }
  load(g_cwd, n);
}

// ---- properties dialog ------------------------------------------------------------------------

static void properties(const std::vector<std::string> &paths_in) {
  std::vector<std::string> paths = paths_in;
  if (paths.empty()) paths.push_back(g_cwd);
  bool single = paths.size() == 1;
  struct stat st{};
  if (single) lstat(paths[0].c_str(), &st);
  Fl_Double_Window *w = new Fl_Double_Window(440, single ? 420 : 200, "Properties");
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
  if (single) {
    row("Modified:", fld::format_time(st.st_mtime, "%Y-%m-%d %H:%M:%S"));
    row("Accessed:", fld::format_time(st.st_atime, "%Y-%m-%d %H:%M:%S"));
    struct passwd *pw = getpwuid(st.st_uid);
    struct group *gr = getgrgid(st.st_gid);
    row("Owner:", std::string(pw ? pw->pw_name : std::to_string(st.st_uid)) + " : " +
                     (gr ? gr->gr_name : std::to_string(st.st_gid)));
    y += 4;
    const char *who[] = {"Owner", "Group", "Others"};
    const char *what[] = {"Read", "Write", "Execute"};
    for (int r = 0; r < 3; r++) {
      Fl_Box *l = new Fl_Box(16, y, 110, 22, who[r]);
      l->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
      l->labelcolor(fld::palette().fg_dim);
      for (int c = 0; c < 3; c++) {
        perm[r * 3 + c] = new Fl_Check_Button(134 + c * 95, y, 90, 22, what[c]);
        perm[r * 3 + c]->value((st.st_mode >> (8 - (r * 3 + c))) & 1);
        if (st.st_uid != getuid() || S_ISLNK(st.st_mode)) perm[r * 3 + c]->deactivate();
      }
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
  }
  Fl::delete_widget(w);
  refresh();
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
  Fl_Check_Button *def = new Fl_Check_Button(12, 342, 356, 24, "Always use for this file type");
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
  std::vector<std::string> paths = selected_paths();
  bool many = paths.size() > 1;
  std::vector<fld::DesktopEntry> apps;
  if (!e.is_dir && !many && !in_trash()) apps = fld::apps_for_mime(e.get_mime());
  std::vector<Fl_Menu_Item> m;
  enum { OPEN = 1, OPEN_TAB, OPEN_TERM, OPEN_WITH_OTHER, CUT, COPY, PASTE, RENAME, TRASH, DELETE, PROPS,
         RESTORE, COPY_PATH, CONTAINING, BOOKMARK, NEW_WINDOW, APP = 100 };
  if (in_trash()) {
    m.push_back({"Restore", 0, nullptr, (void *)RESTORE});
    m.push_back({"Delete Permanently", 0, nullptr, (void *)DELETE});
  } else {
    m.push_back({"Open", 0, nullptr, (void *)OPEN});
    if (in_search() && !many) m.push_back({"Open Containing Folder", 0, nullptr, (void *)CONTAINING});
    if (e.is_dir) m.push_back({"Open in New Window", 0, nullptr, (void *)NEW_WINDOW});
    if (e.is_dir) m.push_back({"Open in Terminal", 0, nullptr, (void *)OPEN_TERM});
    if (e.is_dir && !many) m.push_back({"Add to Bookmarks", 0, nullptr, (void *)BOOKMARK});
    if (!e.is_dir && !many) {
      m.push_back({"Open With", 0, nullptr, nullptr, FL_SUBMENU});
      for (size_t k = 0; k < apps.size() && k < 12; k++) m.push_back({apps[k].name.c_str(), 0, nullptr, (void *)(APP + k)});
      if (!apps.empty()) m.back().flags |= FL_MENU_DIVIDER;
      m.push_back({"Other Application…", 0, nullptr, (void *)OPEN_WITH_OTHER});
      m.push_back({nullptr});
    }
    m.back().flags |= FL_MENU_DIVIDER;
    m.push_back({"Cut", FL_CTRL + 'x', nullptr, (void *)CUT});
    m.push_back({"Copy", FL_CTRL + 'c', nullptr, (void *)COPY});
    if (e.is_dir && !many) m.push_back({"Paste Into Folder", 0, nullptr, (void *)PASTE, g_clip.paths.empty() ? FL_MENU_INACTIVE : 0});
    m.push_back({"Copy Path", 0, nullptr, (void *)COPY_PATH, FL_MENU_DIVIDER});
    if (!many) m.push_back({"Rename…", FL_F + 2, nullptr, (void *)RENAME});
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
    case TRASH: trash_selected(false); break;
    case DELETE: trash_selected(true); break;
    case PROPS: properties(paths); break;
    case RESTORE: restore_selected(); break;
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

void FileView::background_menu() {
  sel.clear();
  redraw();
  update_status();
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
        {"Zoom In", FL_CTRL + '=', nullptr, (void *)23, V.zoom >= 6 ? FL_MENU_INACTIVE : 0},
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
        if (i >= 0 && entry(i).is_dir) fld::spawn({"fl-files", entry(i).path});
        return 1;
      }
      press_item = i;
      press_x = ex;
      press_y = ey;
      dragging_files = false;
      if (i >= 0) {
        if (st & FL_CTRL) {
          if (sel.count(i)) sel.erase(i); else sel.insert(i);
          focus = anchor = i;
        } else if (st & FL_SHIFT && anchor >= 0) {
          sel.clear();
          for (int k = std::min(anchor, i); k <= std::max(anchor, i); k++) sel.insert(k);
          focus = i;
        } else if (!sel.count(i)) {
          sel = {i};
          focus = anchor = i;
        } else focus = i;
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
      if (press_item >= 0 && !dragging_files && !in_trash() &&
          (std::abs(ex - press_x) > 6 || std::abs(ey - press_y) > 6)) {
        // start a drag with file URIs
        dragging_files = true;
        std::string text;
        for (auto &p : selected_paths()) text += "file://" + fld::uri_encode_path(p) + "\r\n";
        Fl::copy(text.c_str(), (int)text.size(), 0);
        Fl::dnd();
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
      // single-click mode opens on release (not after a drag or with modifiers)
      if (S.single_click && Fl::event_button() == FL_LEFT_MOUSE && press_item >= 0 && !dragging_files &&
          !(Fl::event_state() & (FL_CTRL | FL_SHIFT)) && press_item == item_at(Fl::event_x(), Fl::event_y()) &&
          press_item < (int)items.size()) {
        int i = press_item;
        press_item = -1;
        open_entry(entry(i));
      }
      return 1;
    case FL_MOVE:
      if (details() && Fl::event_y() < y() + HEADER_H) {
        bool border;
        int c = header_hit(Fl::event_x(), border);
        window()->cursor(border && c >= 0 ? FL_CURSOR_WE : FL_CURSOR_DEFAULT);
      } else window()->cursor(FL_CURSOR_DEFAULT);
      return 1;
    case FL_LEAVE:
      if (window()) window()->cursor(FL_CURSOR_DEFAULT);
      return 1;
    case FL_DND_ENTER: case FL_DND_DRAG: case FL_DND_RELEASE: return in_trash() ? 0 : 1;
    case FL_DND_LEAVE: return 1;
    case FL_PASTE: {
      // dropped / pasted file URIs: copy (Shift: move) them here or into the folder under the pointer
      if (in_trash()) return 1;
      if (in_search() && item_at(Fl::event_x(), Fl::event_y()) < 0) return 1;
      std::vector<std::string> paths;
      for (auto &line : fld::split(std::string(Fl::event_text(), Fl::event_length()), '\n')) {
        std::string l = fld::trim(line);
        if (fld::starts_with(l, "file://")) l = fld::uri_decode(l.substr(7));
        if (!l.empty() && l[0] == '/' && fld::file_exists(l)) paths.push_back(l);
      }
      if (paths.empty()) return 1;
      std::string dest = g_cwd;
      int i = item_at(Fl::event_x(), Fl::event_y());
      if (Fl::event() == FL_DND_RELEASE || dragging_files) {
        if (i >= 0 && entry(i).is_dir) dest = entry(i).path;
      }
      bool same_dir = true;
      for (auto &p : paths) if (fld::dirname(p) != dest) same_dir = false;
      if (same_dir) return 1;
      bool move = dragging_files || (Fl::event_state() & FL_SHIFT);
      run_op(move ? FileOp::MOVE : FileOp::COPY, paths, dest);
      dragging_files = false;
      return 1;
    }
    case FL_KEYBOARD: {
      int k = Fl::event_key();
      int st = Fl::event_state();
      bool shift = st & FL_SHIFT, ctrl = st & FL_CTRL;
      int cols = columns();
      int page = std::max(1, view_h() / (details() ? row_h() : cell_h())) * cols;
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
        case FL_Delete: trash_selected(shift); return 1;
        case FL_F + 2: if (focus >= 0) start_rename(focus); return 1;
        case ' ':
          if (focus >= 0 && ctrl) { if (sel.count(focus)) sel.erase(focus); else sel.insert(focus); redraw(); return 1; }
          break;
      }
      // type-ahead: jump to the first item starting with the typed text
      if (!ctrl && !(st & FL_ALT) && Fl::event_length() > 0 && (unsigned char)Fl::event_text()[0] >= ' ') {
        static time_t last = 0;
        time_t t = time(nullptr);
        if (t - last > 1) type_ahead.clear();
        last = t;
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
      if (!fld::starts_with(line, "file://")) continue;
      size_t sp = line.find(' ');
      std::string p = fld::uri_decode(line.substr(7, sp == std::string::npos ? std::string::npos : sp - 7));
      std::string l = sp == std::string::npos ? fld::basename(p) : line.substr(sp + 1);
      add(l, p, "folder");
      if (!g_place_list.empty() && g_place_list.back().path == p) g_place_list.back().bookmark = true;
    }
  }
  add("File System", "/", "drive-harddisk");
  // mounted removable / extra file systems
  std::string mounts;
  if (fld::read_file("/proc/mounts", mounts)) {
    for (auto &line : fld::split(mounts, '\n')) {
      auto f = fld::split(line, ' ');
      if (f.size() < 2) continue;
      std::string mp = fld::uri_decode(f[1]); // \040 escapes are octal, handled below
      std::string decoded;
      for (size_t i = 0; i < f[1].size(); i++) {
        if (f[1][i] == '\\' && i + 3 < f[1].size()) { decoded += (char)strtol(f[1].substr(i + 1, 3).c_str(), nullptr, 8); i += 3; }
        else decoded += f[1][i];
      }
      mp = decoded;
      if (fld::starts_with(mp, "/media/") || fld::starts_with(mp, "/run/media/") || fld::starts_with(mp, "/mnt/"))
        add(fld::basename(mp), mp, "drive-removable-media");
    }
  }
  g_places->clear();
  for (auto &p : g_place_list) {
    g_places->add(("  " + fld::escape_at(p.label)).c_str());
    g_places->icon(g_places->size(), fld::icon_first({p.icon, "folder"}, 20));
  }
}

static std::string bookmarks_path() { return fld::config_home() + "/gtk-3.0/bookmarks"; }

static void add_bookmark(const std::string &path) {
  if (path.empty() || path == TRASH_URI) return;
  std::string bm, uri = "file://" + fld::uri_encode_path(path);
  fld::read_file(bookmarks_path(), bm);
  for (auto &line : fld::split(bm, '\n'))
    if (line == uri || fld::starts_with(line, uri + " ")) return; // already there
  if (!bm.empty() && bm.back() != '\n') bm += '\n';
  bm += uri + "\n";
  fld::mkdir_p(fld::dirname(bookmarks_path()));
  if (!fld::write_file(bookmarks_path(), bm)) fl_alert("Could not save %s", bookmarks_path().c_str());
  fill_places();
  select_place_for(g_cwd);
}

static void remove_bookmark(const std::string &path) {
  std::string bm, out, uri = "file://" + fld::uri_encode_path(path);
  if (!fld::read_file(bookmarks_path(), bm)) return;
  for (auto &line : fld::split(bm, '\n'))
    if (line != uri && !fld::starts_with(line, uri + " ")) out += line + "\n";
  fld::write_file(bookmarks_path(), out);
  fill_places();
  select_place_for(g_cwd);
}

static void rename_bookmark(const std::string &path, const std::string &old_label) {
  const char *n = fl_input("Bookmark name:", old_label.c_str());
  if (!n) return;
  std::string bm, out, uri = "file://" + fld::uri_encode_path(path);
  if (!fld::read_file(bookmarks_path(), bm)) return;
  for (auto &line : fld::split(bm, '\n')) {
    if (line == uri || fld::starts_with(line, uri + " ")) out += uri + (*n ? " " + std::string(n) : "") + "\n";
    else out += line + "\n";
  }
  fld::write_file(bookmarks_path(), out);
  fill_places();
  select_place_for(g_cwd);
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
      Fl_Menu_Item m[] = {{"Open", 0, nullptr, (void *)1},
                          {"Open in New Window", 0, nullptr, (void *)2, FL_MENU_DIVIDER},
                          {"Rename Bookmark…", 0, nullptr, (void *)3, p.bookmark ? 0 : FL_MENU_INACTIVE},
                          {"Remove Bookmark", 0, nullptr, (void *)4, p.bookmark ? 0 : FL_MENU_INACTIVE},
                          {nullptr}};
      const Fl_Menu_Item *r = m->popup(Fl::event_x(), Fl::event_y());
      if (!r) return 1;
      switch ((long)r->user_data()) {
        case 1: navigate(p.path); break;
        case 2: fld::spawn({"fl-files", p.path}); break;
        case 3: rename_bookmark(p.path, p.label); break;
        case 4: remove_bookmark(p.path); break;
      }
      return 1;
    }
    return Fl_Hold_Browser::handle(e);
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
  std::sort(names.begin(), names.end(), [](auto &a, auto &b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
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
  if (!g_tree || !g_tree->visible() || path == TRASH_URI || in_search()) return;
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
  if (g_cwd == "/" || in_trash()) g_up->deactivate(); else g_up->activate();
  if (in_trash()) g_empty_trash->show(); else g_empty_trash->hide();
}

static void navigate(const std::string &path_in, bool record) {
  std::string path = path_in == TRASH_URI ? path_in : fld::abs_path(path_in);
  if (path != TRASH_URI) {
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
    g_search.cancel();
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
  // when going up, select the folder we came from
  std::string select;
  if (!prev.empty() && prev != TRASH_URI && fld::dirname(prev) == path) select = fld::basename(prev);
  g_view->load(path, select);
  g_path->value(path == TRASH_URI ? "Trash" : path.c_str());
  std::string title = (path == TRASH_URI ? std::string("Trash") : (path == "/" ? "/" : fld::basename(path))) + " – Files";
  g_win->copy_label(title.c_str());
  select_place_for(path);
  tree_follow(path);
  watch_dir(path);
  update_nav_buttons();
  update_status();
  g_view->take_focus();
}

static void refresh() {
  if (in_search()) {
    g_view->restat_all();
    update_status();
    return;
  }
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
  std::string s = std::to_string(g_view->items.size()) + " item" + (g_view->items.size() == 1 ? "" : "s");
  if (in_search()) {
    size_t n = g_view->items.size();
    if (g_search.running())
      s = "Searching " + fld::tilde(g_search.current_dir()) + "…    " + std::to_string(n) + " found";
    else
      s = std::to_string(n) + (n == 1 ? " match" : " matches") + " in " + fld::tilde(g_cwd) +
          (g_search.truncated() ? " (stopped at the result limit)" : "") + "    " +
          std::to_string(g_search.scanned()) + " items searched";
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
    if (statvfs(g_cwd.c_str(), &vf) == 0)
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
  if (in_trash() || g_cwd == "/") return;
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

static void search_poll(void *) {
  std::vector<std::string> paths;
  g_search.take(paths);
  if (!paths.empty()) g_view->add_paths(paths);
  update_status();
  g_view->redraw();
  if (g_search.running()) Fl::repeat_timeout(0.15, search_poll);
  else {
    g_s_go->label("Search");
    // the last batch
    paths.clear();
    g_search.take(paths);
    if (!paths.empty()) g_view->add_paths(paths);
    update_status();
  }
}

static void stop_search() {
  g_search.cancel();
  Fl::remove_timeout(search_poll);
  search_poll(nullptr);
}

static void start_search() {
  if (g_search.running()) {
    stop_search();
    return;
  }
  search::Query q;
  q.root = g_cwd == TRASH_URI ? fld::home_dir() : g_cwd;
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
  if (!g_search.start(q)) {
    fl_alert("The regular expression is not valid.");
    return;
  }
  if (!in_search()) {
    if (g_cwd == TRASH_URI) g_cwd = q.root;
    g_searching = true;
  }
  g_search_desc = q.name.empty() ? (q.content.empty() ? "files" : "\"" + q.content + "\"") : q.name;
  g_view->clear_thumbs();
  g_view->all.clear();
  g_filter->value("");
  g_view->apply_filter_sort();
  std::string title = "Search: " + g_search_desc + " – Files";
  g_win->copy_label(title.c_str());
  g_places->value(0);
  if (g_tree) g_tree->deselect_all(nullptr, 0);
  g_s_go->label("Stop");
  update_nav_buttons();
  Fl::remove_timeout(search_poll);
  Fl::add_timeout(0.1, search_poll);
  update_status();
}

static void show_search_bar(bool on) {
  int W = g_win->w(), TB = 42, ST = 24, H = g_win->h();
  if (on == g_sbar->visible()) {
    if (on) g_s_name->take_focus();
    return;
  }
  if (on) {
    g_sbar->resize(0, TB, W, SBAR_H);
    g_sbar->show();
    g_tile->resize(0, TB + SBAR_H, W, H - TB - ST - SBAR_H);
    g_s_name->take_focus();
    g_s_name->insert_position(0, g_s_name->size());
  } else {
    if (in_search()) navigate(g_cwd, false);
    g_sbar->hide();
    g_tile->resize(0, TB, W, H - TB - ST);
    g_view->take_focus();
  }
  g_tile->init_sizes();
  g_win->redraw();
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
        if (g_search.running()) stop_search();
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
  g_s_hidden = new Fl_Check_Button(x, y, 76, 28, "Hidden");
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
static void zoom(int delta) {
  int z = delta == 0 ? 2 : std::max(0, std::min(6, V.zoom + delta));
  if (z == V.zoom) return;
  V.zoom = z;
  remember_view(in_search() ? "" : g_cwd);
  g_view->clear_images();
  int f = g_view->focus;
  g_view->update_scrollbar();
  g_view->ensure_visible(f);
  g_view->redraw();
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
  Fl_Double_Window *w = new Fl_Double_Window(460, 430, "Files Preferences");
  int y = 14;
  auto heading = [&](const char *t) {
    Fl_Box *b = new Fl_Box(16, y, 428, 22, t);
    b->labelfont(FL_HELVETICA_BOLD);
    b->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    y += 26;
  };
  auto check = [&](const char *t, bool v) {
    Fl_Check_Button *c = new Fl_Check_Button(28, y, 416, 24, t);
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
  y += 6;
  heading("Thumbnails");
  Fl_Check_Button *th = check("Show image thumbnails", S.thumbnails);
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
  w->size(460, y + 44);
  w->end();
  w->set_modal();
  w->show();
  while (w->shown()) Fl::wait();
  bool reload = S.show_hidden != (bool)hid->value() || S.folders_first != (bool)ff->value() ||
                S.thumbnails != (bool)th->value() || S.thumb_max_mb != (int)tmax->value();
  bool view_changed = S.def.view != (ViewMode)view->value();
  S.def.view = (ViewMode)view->value();
  S.date_fmt = date->value();
  S.per_folder = per->value();
  S.folders_first = ff->value();
  S.show_hidden = hid->value();
  S.single_click = one->value();
  S.confirm_trash = conf->value();
  S.thumbnails = th->value();
  S.thumb_max_mb = (int)tmax->value();
  save_settings();
  Fl::delete_widget(w);
  if (view_changed && !S.per_folder) set_view(S.def.view);
  else if (view_changed && !in_search() && folder_view(g_cwd).view == S.def.view) set_view(S.def.view);
  if (reload) refresh();
  g_view->redraw();
}

static void refresh_later(void *) { refresh(); }

static void inotify_cb(int fd, void *) {
  char buf[8192];
  while (read(fd, buf, sizeof buf) > 0) {}
  Fl::remove_timeout(refresh_later);
  Fl::add_timeout(0.25, refresh_later);
}

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
  if (k == FL_F + 3 || (ctrl && shift && (k == 'f' || k == 'F'))) { show_search_bar(true); return 1; }
  if (k == FL_Escape && in_search() && Fl::focus() == g_view) { show_search_bar(false); return 1; }
  if (k == FL_BackSpace && Fl::focus() == g_view) { go_up(); return 1; }
  if (k == FL_F + 5) { refresh(); return 1; }
  if (!ctrl) return 0;
  switch (k) {
    case 'l': g_path->value(in_trash() ? "trash:///" : g_cwd.c_str()); g_path->take_focus(); g_path->insert_position(0, g_path->size()); return 1;
    case 'f': g_filter->take_focus(); return 1;
    case 'h': toggle_hidden(); return 1;
    case 'd': if (!in_search()) add_bookmark(g_cwd); return 1;
    case ',': show_preferences(); return 1;
    case 'n':
      if (shift) new_item(true);
      else fld::spawn({"fl-files", in_trash() ? fld::home_dir() : g_cwd});
      return 1;
    case 'w': case 'q': g_win->hide(); return 1;
    case '1': set_view(VIEW_ICONS); return 1;
    case '2': set_view(VIEW_COMPACT); return 1;
    case '3': set_view(VIEW_DETAILS); return 1;
    case '=': case '+': zoom(1); return 1;
    case '-': zoom(-1); return 1;
    case '0': zoom(0); return 1;
  }
  if (Fl::focus() != g_view) return 0;
  switch (k) {
    case 'a': for (int i = 0; i < (int)g_view->items.size(); i++) g_view->sel.insert(i); g_view->redraw(); update_status(); return 1;
    case 'c': copy_to_clipboard(false); return 1;
    case 'x': copy_to_clipboard(true); return 1;
    case 'v': if (!in_search()) paste_into(g_cwd); return 1;
  }
  return 0;
}

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
  std::string start = fld::home_dir();
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") { printf("Usage: fl-files [folder|trash:///]\n"); return 0; }
    if (a[0] == '-') continue;
    if (a == TRASH_URI || a == "trash:") start = TRASH_URI;
    else if (fld::starts_with(a, "file://")) start = fld::uri_decode(a.substr(7));
    else start = a;
    if (start != TRASH_URI && !fld::is_dir(start)) start = fld::dirname(fld::abs_path(start));
  }

  const int W = 980, H = 640, TB = 42, ST = 24;
  const fld::Palette &p = fld::palette();
  g_win = new FilesWindow(W, H, "Files");

  fld::Toolbar *bar = new fld::Toolbar(0, 0, W, TB);
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
    else navigate(fld::expand_tilde(v));
  });
  g_path->tooltip("Location (Ctrl+L)");
  bar->add(g_path);
  g_filter = new Fl_Input(W - 270, 7, 112, 28);
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
    Fl_Menu_Item m[] = {
        {"New Window", FL_CTRL + 'n', nullptr, (void *)1},
        {"New Folder…", FL_CTRL + FL_SHIFT + 'n', nullptr, (void *)2, FL_MENU_DIVIDER},
        {"Find Files…", FL_CTRL + FL_SHIFT + 'f', nullptr, (void *)8},
        {"Bookmark This Folder", FL_CTRL + 'd', nullptr, (void *)9, FL_MENU_DIVIDER},
        {"View", 0, nullptr, nullptr, FL_SUBMENU},
          {"Icons", FL_CTRL + '1', nullptr, (void *)20, FL_MENU_RADIO | (V.view == VIEW_ICONS ? FL_MENU_VALUE : 0)},
          {"Compact", FL_CTRL + '2', nullptr, (void *)21, FL_MENU_RADIO | (V.view == VIEW_COMPACT ? FL_MENU_VALUE : 0)},
          {"Details", FL_CTRL + '3', nullptr, (void *)22, FL_MENU_RADIO | (V.view == VIEW_DETAILS ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER},
          {"Zoom In", FL_CTRL + '=', nullptr, (void *)23},
          {"Zoom Out", FL_CTRL + '-', nullptr, (void *)24},
          {"Normal Size", FL_CTRL + '0', nullptr, (void *)25, FL_MENU_DIVIDER},
          {"Columns…", 0, nullptr, (void *)26},
          {nullptr},
        {"Folder Tree", FL_F + 9, nullptr, (void *)10, FL_MENU_TOGGLE | (S.show_tree ? FL_MENU_VALUE : 0)},
        {"Show Hidden Files", FL_CTRL + 'h', nullptr, (void *)3, FL_MENU_TOGGLE | (S.show_hidden ? FL_MENU_VALUE : 0)},
        {"Show Thumbnails", 0, nullptr, (void *)4, FL_MENU_TOGGLE | (S.thumbnails ? FL_MENU_VALUE : 0), 0, 0, 0, 0},
        {"Open Terminal Here", 0, nullptr, (void *)5, FL_MENU_DIVIDER},
        {"Preferences…", FL_CTRL + ',', nullptr, (void *)11},
        {"About", 0, nullptr, (void *)6},
        {"Quit", FL_CTRL + 'q', nullptr, (void *)7},
        {nullptr}};
    const Fl_Menu_Item *r = m->popup(Fl::event_x(), Fl::event_y());
    if (!r) return;
    switch ((long)r->user_data()) {
      case 1: fld::spawn({"fl-files", in_trash() ? fld::home_dir() : g_cwd}); break;
      case 2: new_item(true); break;
      case 3: toggle_hidden(); break;
      case 4: S.thumbnails = !S.thumbnails; save_settings(); g_view->clear_thumbs(); refresh(); break;
      case 8: show_search_bar(true); break;
      case 9: if (!in_search()) add_bookmark(g_cwd); break;
      case 10: show_tree(!S.show_tree); break;
      case 11: show_preferences(); break;
      case 20: set_view(VIEW_ICONS); break;
      case 21: set_view(VIEW_COMPACT); break;
      case 22: set_view(VIEW_DETAILS); break;
      case 23: zoom(1); break;
      case 24: zoom(-1); break;
      case 25: zoom(0); break;
      case 26: columns_menu(Fl::event_x(), Fl::event_y()); break;
      case 5: if (!in_trash()) fld::spawn(fld::shell_split(fld::terminal_command()), g_cwd); break;
      case 6: fld::about_dialog("Files", "fl-files", "A file manager built with FLTK."); break;
      case 7: g_win->hide(); break;
    }
  });
  bar->add(menu_btn);
  bar->resizable(g_path);

  build_search_bar(W, TB);
  Fl_Tile *tile = g_tile = new Fl_Tile(0, TB, W, H - TB - ST);
  Fl_Group *side = new Fl_Group(0, TB, S.sidebar_w, H - TB - ST);
  g_tree = new Fl_Tree(0, TB, S.sidebar_w, H - TB - ST - 40);
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
    if (line > 0 && line <= (int)g_place_list.size()) navigate(g_place_list[line - 1].path);
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
  g_view = new FileView(S.sidebar_w, TB, W - S.sidebar_w, H - TB - ST);
  tile->end();
  tile->resizable(g_view);
  tile->size_range(side, 120, 120, 400, 10000);
  g_status = new Fl_Box(0, H - ST, W, ST);
  g_status->box(FL_FLAT_BOX);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(p.fg_dim);
  g_win->resizable(tile);
  g_win->end();
  g_win->size_range(500, 320);
  fld::set_window_icon(g_win, "fl-files");
  Fl::add_handler(global_keys);

  g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (g_inotify >= 0) Fl::add_fd(g_inotify, FL_READ, inotify_cb);

  fill_places();
  update_view_button();
  if (S.show_tree) show_tree(true);
  navigate(start, false);
  g_win->show(); // not show(argc, argv): it resets the theme colors
  g_view->take_focus();
  return Fl::run();
}
