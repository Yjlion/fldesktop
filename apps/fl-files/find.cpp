// fl-files: finding files and folders
#include "files_app.h"

// ---------------------------------------------------------------------------
// Find files and folders

Fl_Group *g_sbar;
static Fl_Input *g_s_name, *g_s_text;
static Fl_Choice *g_s_kind, *g_s_date, *g_s_size;
static Fl_Check_Button *g_s_hidden;
Fl_Button *g_s_go;
Fl_Tile *g_tile;
static bool g_s_case = false, g_s_regex = false, g_s_links = false;
static const int SBAR_H = 42;

// takes the results found so far into the view of the tab that searches
void search_poll(void *d) {
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
fld::Toolbar *g_toolbar;
Fl_Menu_Bar *g_menubar;

void relayout() {
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

void show_search_bar(bool on) {
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

void open_containing(const std::string &path) {
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

void build_search_bar(int W, int Y) {
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

void update_view_button() {
  // the button shows the view it switches to
  const char *next = V.view == VIEW_DETAILS ? "view-grid" : "view-list";
  g_view_btn->icon_name(next, 20);
  if (!fld::icon(next, 20)) g_view_btn->icon_name((std::string(next) + "-symbolic").c_str(), 20);
}

void set_view(ViewMode m) {
  V.view = m;
  remember_view(in_search() ? "" : g_cwd);
  update_view_button();
  g_view->top = 0;
  g_view->update_scrollbar();
  g_view->redraw();
}

void set_sort(SortKey k, bool desc) {
  V.sort = k;
  V.desc = desc;
  remember_view(in_search() ? "" : g_cwd);
  g_view->resort_keep_selection();
}

// delta: +1 / -1, or 0 for the default size
void set_zoom(int z) {
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
void zoom(int delta) { set_zoom(delta == 0 ? kZoomDefault : V.zoom + delta); }

void update_zoom_slider() {
  if (!g_zoom_slider) return;
  g_zoom_slider->value(V.zoom);
  static char tip[32];
  snprintf(tip, sizeof tip, "Icon size: %d px", kZoomPx[V.zoom]);
  g_zoom_slider->tooltip(tip);
}

void columns_menu(int x, int y) {
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

void show_preferences() {
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

void inotify_cb(int fd, void *) {
  char buf[8192];
  while (read(fd, buf, sizeof buf) > 0) {}
  Fl::remove_timeout(refresh_later);
  Fl::add_timeout(0.25, refresh_later);
}
