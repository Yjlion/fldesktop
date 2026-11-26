#include "files_app.h"

// ---- quick look (Space) ------------------------------------------------------

time_t g_type_ahead_time = 0;
static pid_t g_preview_pid = -1;
static std::string g_preview_path;

static void reap_preview(void *) {
  if (g_preview_pid <= 0) return;
  if (waitpid(g_preview_pid, nullptr, WNOHANG) != 0) g_preview_pid = -1; // ended (or not our child)
  else Fl::repeat_timeout(1.0, reap_preview);
}

// Space shows the file in its viewer; Space on the same file again closes it
void quick_look(const std::string &path) {
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



// a folder of a GVfs mount (a network share, a phone)
bool is_remote(const std::string &path) {
#ifdef FLD_HAVE_GVFS
  return gv::is_fuse_path(path) || gv::Client::get().state().mount_for_path(path);
#else
  (void)path;
  return false;
#endif
}
// the location a folder of a GVfs mount stands for ("" for other folders)
std::string remote_uri(const std::string &path) {
#ifdef FLD_HAVE_GVFS
  return gv::uri_for_path(gv::Client::get().state(), path);
#else
  (void)path;
  return "";
#endif
}
// the name of the GVfs mount whose root path is ("" for other folders)
std::string remote_mount_name(const std::string &path) {
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


// Details view columns besides Name







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
ViewState V;

Settings S;

std::string settings_path() { return fld::fld_config_dir() + "/files.ini"; }
std::string folder_settings_path() { return fld::fld_config_dir() + "/files-folders.ini"; }

void save_settings() {
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
ViewState folder_view(const std::string &dir) {
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

void remember_view(const std::string &dir) {
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

std::string format_date(time_t t) {
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

std::string perm_string(mode_t m) {
  std::string s = S_ISDIR(m) ? "d" : S_ISLNK(m) ? "l" : "-";
  const char *rwx = "rwxrwxrwx";
  for (int i = 0; i < 9; i++) s += (m & (1 << (8 - i))) ? rwx[i] : '-';
  if (m & S_ISUID) s[3] = (m & S_IXUSR) ? 's' : 'S';
  if (m & S_ISGID) s[6] = (m & S_IXGRP) ? 's' : 'S';
  if (m & S_ISVTX) s[9] = (m & S_IXOTH) ? 't' : 'T';
  return s;
}

const std::string &owner_name(uid_t uid) {
  static std::map<uid_t, std::string> cache;
  auto it = cache.find(uid);
  if (it != cache.end()) return it->second;
  struct passwd *pw = getpwuid(uid);
  return cache[uid] = pw ? pw->pw_name : std::to_string(uid);
}

// ---------------------------------------------------------------------------
// Globals

class FileView;
Fl_Double_Window *g_win;
FileView *g_view;
Fl_Input *g_path;
Fl_Input *g_filter;
Fl_Box *g_status;
Fl_Slider *g_zoom_slider;
class PlacesBrowser;
Fl_Hold_Browser *g_places;
class Fl_Tree;
Fl_Tree *g_tree;
bool g_searching = false; // the view shows search results
fld::FlatButton *g_back, *g_fwd, *g_up, *g_empty_trash, *g_view_btn;
std::string g_cwd;
std::deque<std::string> g_hist_back, g_hist_fwd;
int g_inotify = -1, g_watch = -1;

Clip g_clip;

search::Search *g_search;  // the current tab's
std::string g_search_desc; // e.g. "*.txt" for the title and status
bool g_search_running() { return g_search->running(); }

// Tabs: each has its own view, folder, history, view settings and search.
// The globals above (g_view, g_cwd, g_hist_*, g_searching, g_search*, V) are
// the current tab's; activate_tab() stores them and loads another tab's.
std::vector<std::unique_ptr<Tab>> g_tabs;
int g_cur = -1;
fld::TabBar *g_tabbar;
Fl_Group *g_views; // holds the tabs' views (one shown)
Tab *cur_tab() { return g_cur >= 0 && g_cur < (int)g_tabs.size() ? g_tabs[(size_t)g_cur].get() : nullptr; }


bool in_trash() { return g_cwd == TRASH_URI && !g_searching; }
bool in_network() { return g_cwd == NETWORK_URI && !g_searching; }
// the network view: what it lists, and why it may be empty
std::vector<Entry> g_net_entries;
// shares that announce themselves (mDNS): also without gvfsd-dnssd
std::vector<Entry> g_mdns_entries;
std::map<std::string, std::string> g_mdns_names; // target -> the server's own name

// what the Network view lists: GVfs' neighbourhood, then the mDNS shares it lacks
std::vector<Entry> network_entries() {
  std::vector<Entry> v = g_net_entries;
  for (auto &m : g_mdns_entries) {
    bool dup = false;
    for (auto &e : g_net_entries) dup |= e.target == m.target || e.name == m.name || e.name == g_mdns_names[m.target];
    if (!dup) v.push_back(m);
  }
  return v;
}
bool g_net_listing = false;
std::string g_net_error;
bool in_search() { return g_searching; }

// Split view: two tabs side by side; clicking in a pane makes its tab current.
bool g_split = false;
Tab *g_pane[2] = {nullptr, nullptr}; // left and right while split
int g_pane_focus = 0;
Tab *tab_of(const void *view) {
  for (auto &t : g_tabs)
    if ((const void *)t->view == view) return t.get();
  return nullptr;
}
int tab_index(const Tab *t) {
  for (size_t k = 0; k < g_tabs.size(); k++)
    if (g_tabs[k].get() == t) return (int)k;
  return -1;
}
// While the other pane's view draws or takes an event, the globals that
// describe "the current tab" (view settings, folder, search) are its tab's.

// ---------------------------------------------------------------------------
// The file view (icon grid or detail list)


// ---------------------------------------------------------------------------
// Menus: the toolbar's menu button and the menu bar (View > Show Menu Bar)

void toggle_menubar() {
  S.menubar = !S.menubar;
  save_settings();
  relayout();
}

void select_all() {
  for (int i = 0; i < (int)g_view->items.size(); i++) g_view->sel.insert(i);
  g_view->redraw();
  update_status();
}

void edit_location() {
  g_path->value(in_trash() ? "trash:///" : g_cwd.c_str());
  g_path->take_focus();
  g_path->insert_position(0, g_path->size());
}

std::string new_tab_dir() { return in_trash() || in_search() || g_cwd.empty() ? fld::home_dir() : g_cwd; }

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

void rebuild_menubar() { ((FilesMenuBar *)g_menubar)->rebuild(); }

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
