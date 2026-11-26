// fl-files: tabs
#include "files_app.h"

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
void toggle_split() {
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
std::string other_pane_dir() {
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

void activate_tab(int i) { activate_tab_(i, true); }

void update_tabbar() {
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

void new_tab(const std::string &path, bool activate) {
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

void close_tab(int i) {
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

void open_dir_elsewhere(const std::string &path) {
  if (S.middle_tab) new_tab(path, false);
  else fld::spawn({"fl-files", path});
}

void leave_folder(const std::string &dir) {
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

fld::TabBar *make_tabbar(int W) {
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
std::string export_tab(int i) {
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
bool import_tab(const std::string &state, int at) {
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
