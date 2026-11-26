// fl-files: the folder tree in the sidebar
#include "files_app.h"

// ---------------------------------------------------------------------------
// Folder tree (sidebar)

std::map<Fl_Tree_Item *, std::string> g_tree_paths;
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
void tree_follow(const std::string &path) {
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

void tree_cb(Fl_Widget *, void *) {
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

void show_tree(bool on) {
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

void select_place_for(const std::string &path) {
  int best = 0;
  for (size_t i = 0; i < g_place_list.size(); i++)
    if (g_place_list[i].path == path) best = (int)i + 1;
  g_places->value(best);
}

void watch_dir(const std::string &dir) {
  if (g_inotify < 0) return;
  if (g_watch >= 0) inotify_rm_watch(g_inotify, g_watch);
  std::string d = dir == TRASH_URI ? fld::trash_dir() + "/files" : dir;
  g_watch = inotify_add_watch(g_inotify, d.c_str(),
                              IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_ATTRIB | IN_CLOSE_WRITE);
}

void update_nav_buttons() {
  if (g_hist_back.empty() && !in_search()) g_back->deactivate(); else g_back->activate();
  if (g_hist_fwd.empty()) g_fwd->deactivate(); else g_fwd->activate();
  if (g_cwd == "/" || in_trash() || in_network()) g_up->deactivate(); else g_up->activate();
  if (in_trash()) g_empty_trash->show(); else g_empty_trash->hide();
}

// The name of a tab's folder (or search) for the title and the tab
std::string location_name(const std::string &cwd, bool searching, const std::string &desc) {
  if (searching) return "Search: " + desc;
  if (cwd == TRASH_URI) return "Trash";
  if (cwd == NETWORK_URI) return "Network";
  std::string mount = remote_mount_name(cwd);
  if (!mount.empty()) return mount;
  if (cwd == "/" || cwd.empty()) return "/";
  return fld::basename(cwd);
}

// The path field, window title and selected place for the current tab
void show_location() {
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

void navigate(const std::string &path_in, bool record) {
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

void refresh() {
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

void update_status() {
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

void go_back() {
  if (in_search()) { navigate(g_cwd, false); return; }
  if (g_hist_back.empty()) return;
  g_hist_fwd.push_back(g_cwd);
  std::string p = g_hist_back.back();
  g_hist_back.pop_back();
  navigate(p, false);
}

void go_forward() {
  if (g_hist_fwd.empty()) return;
  g_hist_back.push_back(g_cwd);
  std::string p = g_hist_fwd.back();
  g_hist_fwd.pop_back();
  navigate(p, false);
}

void go_up() {
  if (in_search()) { navigate(g_cwd, false); return; }
  if (in_trash() || in_network() || g_cwd == "/") return;
  // up from a network location's top folder: the network
  if (!remote_mount_name(g_cwd).empty()) { navigate(NETWORK_URI); return; }
  navigate(fld::dirname(g_cwd));
}

void toggle_hidden() {
  S.show_hidden = !S.show_hidden;
  save_settings();
  g_view->apply_filter_sort();
  if (g_tree && g_tree->visible()) {
    tree_build();
    tree_follow(g_cwd);
  }
  update_status();
}
