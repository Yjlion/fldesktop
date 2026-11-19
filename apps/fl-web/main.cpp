// fl-web: a web browser built with FLTK and WPE WebKit
//
// Tabs, an address bar that searches, bookmarks, history, downloads, find in
// page, zoom, and the page's dialogs (alerts, file choosers, passwords,
// certificates, permissions, notifications) as FLTK dialogs.
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "browser_util.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/mime.h"
#include "fld/notify.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "glib_loop.h"
#include "webview.h"

using namespace web;

// ---------------------------------------------------------------------------
// Settings and shared state

struct Config {
  std::string home = "https://duckduckgo.com/";
  std::string search = "DuckDuckGo";
  bool javascript = true;
  bool restore = false; // reopen the last session's tabs
  void load() {
    fld::Ini c;
    c.load(path());
    home = c.get("Web", "HomePage", home);
    search = c.get("Web", "SearchEngine", search);
    javascript = c.get_bool("Web", "JavaScript", javascript);
    restore = c.get_bool("Web", "RestoreTabs", restore);
  }
  void save() const {
    fld::Ini c;
    c.load(path());
    c.set("Web", "HomePage", home);
    c.set("Web", "SearchEngine", search);
    c.set_bool("Web", "JavaScript", javascript);
    c.set_bool("Web", "RestoreTabs", restore);
    c.save(path());
  }
  static std::string path() { return fld::fld_config_dir() + "/web.ini"; }
} g_cfg;

static std::string data_dir() { return fld::data_home() + "/fl-web"; }
static std::string bookmarks_path() { return data_dir() + "/bookmarks.tsv"; }
static std::string history_path() { return data_dir() + "/history.tsv"; }
static std::string session_path() { return data_dir() + "/session.txt"; }

static WebKitNetworkSession *g_session;
static WebKitSettings *g_settings;
static WebKitUserContentManager *g_ucm;
static std::vector<Bookmark> g_bookmarks;
static std::vector<HistoryEntry> g_history;

struct Download {
  WebKitDownload *dl;
  std::string path, name, error;
  bool done = false, failed = false;
};
static std::vector<std::unique_ptr<Download>> g_downloads;

static bool is_bookmarked(const std::string &url) {
  return std::any_of(g_bookmarks.begin(), g_bookmarks.end(), [&](const Bookmark &b) { return b.url == url; });
}

class Browser;
static std::vector<Browser *> g_windows;
static Browser *new_window(const std::vector<std::string> &urls = {});
static void show_downloads();
static void show_history(Browser *b);
static void show_bookmarks(Browser *b);
static void show_settings();

// ---------------------------------------------------------------------------
// Tabs

struct Tab {
  Browser *win;
  WebView *wv;
  std::string title, uri;
  double progress = 0;
  bool loading = false;
  std::string hover;
  std::string typed; // address bar text being edited while the tab was in the background
};

// The tab strip: tabs share the width, each with a close button; "+" at the end
class TabBar : public Fl_Widget {
public:
  std::vector<Tab *> *tabs = nullptr;
  int current = 0;
  std::function<void(int)> on_select, on_close;
  std::function<void()> on_new;
  TabBar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}

  int tab_w() const {
    int n = tabs ? (int)tabs->size() : 1;
    return std::max(60, std::min(240, (w() - 40) / std::max(1, n)));
  }
  int plus_x() const { return x() + (tabs ? (int)tabs->size() : 0) * tab_w() + 4; }
  int hit(int ex, bool &close) const {
    close = false;
    if (!tabs) return -1;
    int i = (ex - x()) / tab_w();
    if (ex < x() || i >= (int)tabs->size()) return -1;
    int tx = x() + i * tab_w();
    close = ex >= tx + tab_w() - 24;
    return i;
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_color(p.bg);
    fl_rectf(x(), y(), w(), h());
    if (!tabs) return;
    fl_font(FL_HELVETICA, 12);
    for (int i = 0; i < (int)tabs->size(); i++) {
      Tab *t = (*tabs)[(size_t)i];
      int tx = x() + i * tab_w(), tw = tab_w();
      bool cur = i == current;
      fl_color(cur ? p.bg2 : p.bg);
      fl_rectf(tx + 1, y() + 3, tw - 2, h() - 3);
      if (cur) {
        fl_color(p.accent);
        fl_rectf(tx + 1, y() + 3, tw - 2, 2);
      } else {
        fl_color(p.border);
        fl_yxline(tx + tw - 1, y() + 8, y() + h() - 6);
      }
      if (t->loading) {
        fl_color(p.accent);
        fl_arc(tx + 8, y() + h() / 2 - 6, 12, 12, 0, 300);
      }
      std::string label = t->title.empty() ? (t->uri.empty() ? "New Tab" : fallback_title(t->uri)) : t->title;
      fl_color(cur ? p.fg : p.fg_dim);
      int lx = tx + (t->loading ? 26 : 10);
      fl_push_clip(lx, y(), tw - (lx - tx) - 26, h());
      fl_draw(fld::escape_at(label).c_str(), lx, y() + 3, 10000, h() - 3, FL_ALIGN_LEFT);
      fl_pop_clip();
      fl_color(p.fg_dim);
      int cx = tx + tw - 18, cy = y() + 3 + (h() - 3) / 2;
      fl_line(cx - 4, cy - 4, cx + 4, cy + 4);
      fl_line(cx - 4, cy + 4, cx + 4, cy - 4);
    }
    fl_color(p.fg_dim);
    int px = plus_x() + 14, py = y() + 3 + (h() - 3) / 2;
    fl_xyline(px - 6, py, px + 6);
    fl_yxline(px, py - 6, py + 6);
  }
  int handle(int e) override {
    if (e == FL_PUSH) {
      bool close;
      int i = hit(Fl::event_x(), close);
      if (i < 0) {
        if (Fl::event_x() >= plus_x() && Fl::event_x() < plus_x() + 28 && on_new) on_new();
        else if (Fl::event_clicks() && on_new) on_new(); // double-click on the empty strip
        return 1;
      }
      if ((close && Fl::event_button() == FL_LEFT_MOUSE) || Fl::event_button() == FL_MIDDLE_MOUSE) {
        if (on_close) on_close(i);
      } else if (on_select) on_select(i);
      return 1;
    }
    if (e == FL_MOUSEWHEEL && tabs && !tabs->empty()) {
      int n = (int)tabs->size();
      if (on_select) on_select(((current + (Fl::event_dy() > 0 ? 1 : -1)) % n + n) % n);
      return 1;
    }
    return Fl_Widget::handle(e);
  }
};

// The address bar: completes from bookmarks and history as you type
class UrlBar : public Fl_Input {
public:
  using Fl_Input::Fl_Input;
  std::function<void()> on_escape;
  int handle(int e) override {
    if (e == FL_FOCUS) {
      int r = Fl_Input::handle(e);
      insert_position(size(), 0); // select all
      return r;
    }
    if (e == FL_KEYBOARD) {
      int k = Fl::event_key();
      if (k == FL_Escape) {
        if (on_escape) on_escape();
        return 1;
      }
      bool typed = Fl::event_length() > 0 && (unsigned char)Fl::event_text()[0] >= 0x20 &&
                   !(Fl::event_state() & (FL_CTRL | FL_ALT | FL_META));
      int r = Fl_Input::handle(e);
      if (typed && insert_position() == size()) complete();
      return r;
    }
    return Fl_Input::handle(e);
  }
  // inline completion: the rest of the best match, selected
  void complete() {
    std::string t = value();
    if (t.size() < 2) return;
    for (auto &s : suggestions(t, g_bookmarks, g_history, 8)) {
      std::string u = s.url;
      for (const char *p : {"https://", "http://"})
        if (fld::starts_with(u, p)) u = u.substr(strlen(p));
      if (fld::starts_with(u, "www.") && !fld::starts_with(t, "www.")) u = u.substr(4);
      if (fld::starts_with(fld::lower(u), fld::lower(t)) && u.size() > t.size()) {
        // only up to the end of the host or the next path segment
        size_t end = u.find('/', t.size());
        std::string add = u.substr(t.size(), end == std::string::npos ? std::string::npos : end - t.size() + 1);
        int pos = insert_position();
        insert(add.c_str());
        insert_position(size(), pos);
        return;
      }
    }
  }
};

// ---------------------------------------------------------------------------
// A browser window

class BrowserWindow : public Fl_Double_Window {
public:
  Browser *b;
  BrowserWindow(int w, int h, Browser *br) : Fl_Double_Window(w, h, "Web"), b(br) {}
  int handle(int e) override;
  void hide() override;
};

class Browser {
public:
  BrowserWindow *win;
  fld::Toolbar *bar;
  fld::FlatButton *back, *fwd, *reload, *home, *star, *menu;
  UrlBar *url;
  TabBar *tabbar;
  Fl_Box *progress;
  Fl_Group *content;
  Fl_Group *findbar;
  Fl_Input *find_input;
  Fl_Box *find_count;
  Fl_Box *status;
  std::vector<Tab *> tabs;
  int cur = -1;
  bool fullscreen = false;
  static const int TAB_H = 32, TB = 42, FIND_H = 38;

  Browser() {
    const fld::Palette &p = fld::palette();
    int sx, sy, sw, sh;
    Fl::screen_work_area(sx, sy, sw, sh);
    int W = std::min(1100, sw - 40), H = std::min(760, sh - 40);
    win = new BrowserWindow(W, H, this);
    tabbar = new TabBar(0, 0, W, TAB_H);
    tabbar->tabs = &tabs;
    tabbar->on_select = [this](int i) { select(i); };
    tabbar->on_close = [this](int i) { close_tab(i); };
    tabbar->on_new = [this] { new_tab("", true); };
    bar = new fld::Toolbar(0, TAB_H, W, TB);
    bar->color(p.bg2);
    back = bar->add_button("go-previous", "Back (Alt+Left)", [](Fl_Widget *, void *d) { ((Browser *)d)->go(-1); }, this);
    fwd = bar->add_button("go-next", "Forward (Alt+Right)", [](Fl_Widget *, void *d) { ((Browser *)d)->go(1); }, this);
    reload = bar->add_button("view-refresh", "Reload (F5)", [](Fl_Widget *, void *d) { ((Browser *)d)->reload_or_stop(); }, this);
    home = bar->add_button("go-home", "Home (Alt+Home)", [](Fl_Widget *, void *d) { ((Browser *)d)->load(g_cfg.home); }, this);
    int ux = bar->next_x() + 4;
    url = new UrlBar(ux, TAB_H + 7, W - ux - 88, 28);
    url->when(FL_WHEN_ENTER_KEY_ALWAYS);
    url->callback([](Fl_Widget *, void *d) { ((Browser *)d)->url_entered(); }, this);
    url->on_escape = [this] {
      if (Tab *t = tab()) url->value(t->uri.c_str());
      if (Tab *t = tab()) t->wv->take_focus();
    };
    url->tooltip("Address or search (Ctrl+L)");
    bar->add(url);
    star = new fld::FlatButton(W - 82, TAB_H + 3, 36, 36);
    star->color(p.bg2);
    star->tooltip("Bookmark this page (Ctrl+D)");
    star->callback([](Fl_Widget *, void *d) { ((Browser *)d)->toggle_bookmark(); }, this);
    bar->add(star);
    menu = new fld::FlatButton(W - 42, TAB_H + 3, 36, 36);
    menu->color(p.bg2);
    menu->icon_name("open-menu", 20);
    menu->tooltip("Menu");
    menu->callback([](Fl_Widget *, void *d) { ((Browser *)d)->main_menu(); }, this);
    bar->add(menu);
    bar->resizable(url);
    bar->end();
    progress = new Fl_Box(0, TAB_H + TB, 0, 2);
    progress->box(FL_FLAT_BOX);
    progress->color(p.accent);
    content = new Fl_Group(0, TAB_H + TB + 2, W, H - TAB_H - TB - 2);
    content->end();
    findbar = new Fl_Group(0, H - FIND_H, W, FIND_H);
    findbar->box(FL_FLAT_BOX);
    findbar->color(p.bg2);
    find_input = new Fl_Input(10, H - FIND_H + 5, 260, 28);
    find_input->when(FL_WHEN_CHANGED | FL_WHEN_ENTER_KEY_ALWAYS);
    find_input->callback([](Fl_Widget *, void *d) {
      Browser *b = (Browser *)d;
      if (Fl::callback_reason() == FL_REASON_ENTER_KEY || Fl::event_key() == FL_Enter) b->find_next(!(Fl::event_state() & FL_SHIFT));
      else b->find_start();
    }, this);
    Fl_Button *prev = new Fl_Button(276, H - FIND_H + 5, 32, 28, "@8>");
    prev->tooltip("Previous (Shift+F3)");
    prev->callback([](Fl_Widget *, void *d) { ((Browser *)d)->find_next(false); }, this);
    Fl_Button *next = new Fl_Button(310, H - FIND_H + 5, 32, 28, "@2>");
    next->tooltip("Next (F3)");
    next->callback([](Fl_Widget *, void *d) { ((Browser *)d)->find_next(true); }, this);
    find_count = new Fl_Box(350, H - FIND_H + 5, 200, 28);
    find_count->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    find_count->labelcolor(p.fg_dim);
    Fl_Box *fill = new Fl_Box(560, H - FIND_H, W - 600, FIND_H);
    Fl_Button *fclose = new Fl_Button(W - 38, H - FIND_H + 5, 28, 28, "@1+");
    fclose->box(FL_FLAT_BOX);
    fclose->color(p.bg2);
    fclose->tooltip("Close (Esc)");
    fclose->callback([](Fl_Widget *, void *d) { ((Browser *)d)->find_close(); }, this);
    findbar->resizable(fill);
    findbar->end();
    findbar->hide();
    status = new Fl_Box(0, H - 22, 10, 22);
    status->box(FL_FLAT_BOX);
    status->color(p.bg2);
    status->labelsize(12);
    status->labelcolor(p.fg);
    status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
    status->hide();
    win->resizable(content);
    win->end();
    win->size_range(400, 300);
    fld::set_window_icon(win, "fl-web");
    g_windows.push_back(this);
  }

  Tab *tab() const { return cur >= 0 && cur < (int)tabs.size() ? tabs[(size_t)cur] : nullptr; }
  WebKitWebView *view() const { return tab() ? tab()->wv->view() : nullptr; }

  void layout() {
    int W = win->w(), H = win->h();
    int top = fullscreen ? 0 : TAB_H + TB + 2;
    int bottom = findbar->visible() ? FIND_H : 0;
    content->resize(0, top, W, H - top - bottom);
    for (auto *t : tabs) t->wv->resize(0, top, W, H - top - bottom);
    findbar->resize(0, H - FIND_H, W, FIND_H);
    win->redraw();
  }

  // ---- tabs
  Tab *new_tab(const std::string &uri, bool switch_to, WebKitWebView *related = nullptr, int at = -1) {
    Tab *t = new Tab;
    t->win = this;
    content->begin();
    t->wv = new WebView(content->x(), content->y(), content->w(), content->h(), related, g_settings, g_session, g_ucm);
    content->end();
    t->wv->hide();
    t->wv->key_filter = [this](int k, int s) { return shortcut(k, s); };
    connect(t);
    if (at < 0 || at > (int)tabs.size()) tabs.push_back(t);
    else tabs.insert(tabs.begin() + at, t);
    if (!uri.empty()) {
      t->uri = uri;
      webkit_web_view_load_uri(t->wv->view(), uri.c_str());
    }
    if (switch_to || cur < 0) select((int)(std::find(tabs.begin(), tabs.end(), t) - tabs.begin()));
    else if (at >= 0 && at <= cur) cur++;
    if (switch_to && uri.empty()) {
      url->value("");
      url->take_focus();
    }
    tabbar->current = cur;
    tabbar->redraw();
    glib_loop_poke();
    return t;
  }

  void select(int i) {
    if (i < 0 || i >= (int)tabs.size()) return;
    if (Tab *old = tab()) {
      if (Fl::focus() == url) old->typed = url->value();
      if (old != tabs[(size_t)i]) {
        old->wv->hide();
        old->wv->activate_view(false);
      }
    }
    cur = i;
    Tab *t = tab();
    t->wv->resize(content->x(), content->y(), content->w(), content->h());
    t->wv->show();
    t->wv->activate_view(true);
    tabbar->current = i;
    tabbar->redraw();
    update_chrome();
    url->value(!t->typed.empty() ? t->typed.c_str() : t->uri.c_str());
    t->typed.clear();
    if (t->uri.empty()) url->take_focus();
    else t->wv->take_focus();
    if (findbar->visible()) find_start();
    glib_loop_poke();
  }

  void close_tab(int i) {
    if (i < 0 || i >= (int)tabs.size()) return;
    Tab *t = tabs[(size_t)i];
    g_signal_handlers_disconnect_by_data(t->wv->view(), t);
    tabs.erase(tabs.begin() + i);
    content->remove(t->wv);
    Fl::delete_widget(t->wv);
    delete t;
    if (tabs.empty()) {
      cur = -1;
      win->hide();
      return;
    }
    if (cur > i || cur >= (int)tabs.size()) cur--;
    cur = std::max(0, cur);
    int c = cur;
    cur = -1; // the tab we left is gone
    select(c);
  }

  // ---- navigation
  void load(const std::string &uri) {
    if (uri.empty()) return;
    Tab *t = tab();
    if (!t) { new_tab(uri, true); return; }
    t->uri = uri;
    url->value(uri.c_str());
    webkit_web_view_load_uri(t->wv->view(), uri.c_str());
    t->wv->take_focus();
    glib_loop_poke();
  }
  void url_entered() {
    std::string u = url_from_input(url->value(), search_template(g_cfg.search));
    if (!u.empty()) load(u);
  }
  void go(int dir) {
    if (!view()) return;
    if (dir < 0) webkit_web_view_go_back(view());
    else webkit_web_view_go_forward(view());
    glib_loop_poke();
  }
  void reload_or_stop(bool bypass = false) {
    if (!view()) return;
    if (tab()->loading) webkit_web_view_stop_loading(view());
    else if (bypass) webkit_web_view_reload_bypass_cache(view());
    else webkit_web_view_reload(view());
    glib_loop_poke();
  }
  void zoom(int dir) {
    if (!view()) return;
    static const double steps[] = {0.3, 0.5, 0.67, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
    double z = webkit_web_view_get_zoom_level(view());
    double nz = 1.0;
    if (dir > 0) { nz = steps[12]; for (double s : steps) if (s > z + 0.01) { nz = s; break; } }
    else if (dir < 0) { nz = steps[0]; for (double s : steps) if (s < z - 0.01) nz = s; }
    webkit_web_view_set_zoom_level(view(), nz);
    set_status("Zoom " + std::to_string((int)(nz * 100 + 0.5)) + " %", 1.5);
    glib_loop_poke();
  }

  // ---- chrome
  void update_chrome() {
    Tab *t = tab();
    if (!t) return;
    WebKitWebView *v = t->wv->view();
    webkit_web_view_can_go_back(v) ? back->activate() : back->deactivate();
    webkit_web_view_can_go_forward(v) ? fwd->activate() : fwd->deactivate();
    reload->icon_name(t->loading ? "process-stop" : "view-refresh", 20);
    reload->tooltip(t->loading ? "Stop (Esc)" : "Reload (F5)");
    star->icon_name(is_bookmarked(t->uri) ? "starred" : "non-starred", 20);
    int pw = t->loading ? (int)(win->w() * std::max(0.05, t->progress)) : 0;
    progress->resize(0, TAB_H + TB, pw, 2);
    progress->parent()->redraw();
    std::string title = (t->title.empty() ? (t->uri.empty() ? std::string("New Tab") : fallback_title(t->uri)) : t->title) + " — Web";
    win->copy_label(title.c_str());
    if (Fl::focus() != url) url->value(t->uri.c_str());
    bar->redraw();
    tabbar->redraw();
  }
  std::string status_text;
  void set_status(const std::string &s, double secs = 0) {
    status_text = fld::escape_at(s);
    Fl::remove_timeout(hide_status, this);
    if (s.empty()) { status->hide(); win->redraw(); return; }
    fl_font(FL_HELVETICA, 12);
    int w = std::min(win->w() - 20, (int)fl_width(status_text.c_str()) + 16);
    int y = content->y() + content->h() - 22;
    status->resize(0, y, w, 22);
    status->label(status_text.c_str());
    status->show();
    win->redraw();
    if (secs > 0) Fl::add_timeout(secs, hide_status, this);
  }
  static void hide_status(void *d) { ((Browser *)d)->set_status(""); }

  // ---- find in page
  void find_open() {
    if (!findbar->visible()) {
      findbar->show();
      layout();
    }
    find_input->take_focus();
    find_input->insert_position(find_input->size(), 0);
    if (*find_input->value()) find_start();
  }
  void find_close() {
    if (view()) webkit_find_controller_search_finish(webkit_web_view_get_find_controller(view()));
    findbar->hide();
    layout();
    if (tab()) tab()->wv->take_focus();
    glib_loop_poke();
  }
  void find_start() {
    if (!view()) return;
    WebKitFindController *fc = webkit_web_view_get_find_controller(view());
    if (!*find_input->value()) {
      webkit_find_controller_search_finish(fc);
      find_count->label("");
      return;
    }
    webkit_find_controller_search(fc, find_input->value(),
                                  WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, 1000);
    glib_loop_poke();
  }
  void find_next(bool forward) {
    if (!view() || !*find_input->value()) return;
    WebKitFindController *fc = webkit_web_view_get_find_controller(view());
    const char *cur_text = webkit_find_controller_get_search_text(fc);
    if (!cur_text || strcmp(cur_text, find_input->value())) { find_start(); return; }
    if (forward) webkit_find_controller_search_next(fc);
    else webkit_find_controller_search_previous(fc);
    glib_loop_poke();
  }
  std::string find_label;

  // ---- bookmarks
  void toggle_bookmark() {
    Tab *t = tab();
    if (!t || t->uri.empty()) return;
    auto it = std::find_if(g_bookmarks.begin(), g_bookmarks.end(), [&](const Bookmark &b) { return b.url == t->uri; });
    if (it != g_bookmarks.end()) g_bookmarks.erase(it);
    else g_bookmarks.push_back({t->uri, t->title.empty() ? fallback_title(t->uri) : t->title});
    save_bookmarks(bookmarks_path(), g_bookmarks);
    for (auto *w : g_windows) w->update_chrome();
  }

  // ---- menus
  void main_menu() {
    std::vector<Fl_Menu_Item> m;
    auto item = [&](const char *l, int sc, long id, int flags = 0) { m.push_back({l, sc, nullptr, (void *)id, flags}); };
    item("New Tab", FL_CTRL + 't', 1);
    item("New Window", FL_CTRL + 'n', 2);
    item("Open File…", FL_CTRL + 'o', 3, FL_MENU_DIVIDER);
    item("Find in Page…", FL_CTRL + 'f', 4);
    item("Zoom In", FL_CTRL + '=', 5);
    item("Zoom Out", FL_CTRL + '-', 6);
    item("Actual Size", FL_CTRL + '0', 7, FL_MENU_DIVIDER);
    // bookmarks submenu
    m.push_back({"Bookmarks", 0, nullptr, nullptr, FL_SUBMENU});
    bool marked = tab() && is_bookmarked(tab()->uri);
    item(marked ? "Remove Bookmark" : "Bookmark This Page", FL_CTRL + 'd', 8);
    item("Manage Bookmarks…", FL_CTRL + FL_SHIFT + 'o', 9, g_bookmarks.empty() ? 0 : FL_MENU_DIVIDER);
    static std::vector<std::string> labels;
    labels.clear();
    labels.reserve(g_bookmarks.size());
    for (size_t i = 0; i < g_bookmarks.size() && i < 30; i++) {
      std::string l = g_bookmarks[i].title.empty() ? g_bookmarks[i].url : g_bookmarks[i].title;
      if (l.size() > 60) l = l.substr(0, 57) + "…";
      // '/' and '&' are special in menu labels
      std::string esc;
      for (char c : l) { if (c == '/' || c == '\\' || c == '&' || c == '_') esc += '\\'; esc += c; }
      labels.push_back(fld::escape_at(esc));
      item(labels.back().c_str(), 0, 1000 + (long)i);
    }
    m.push_back({nullptr});
    item("History…", FL_CTRL + 'h', 10);
    item("Downloads…", FL_CTRL + 'j', 11, FL_MENU_DIVIDER);
    item("Settings…", 0, 12);
    item("About Web", 0, 13, FL_MENU_DIVIDER);
    item("Close Window", FL_CTRL + FL_SHIFT + 'w', 14);
    item("Quit", FL_CTRL + 'q', 15);
    m.push_back({nullptr});
    const Fl_Menu_Item *r = m[0].popup(menu->x(), menu->y() + menu->h());
    if (!r || !r->user_data()) return;
    long id = (long)r->user_data();
    switch (id) {
      case 1: new_tab("", true); break;
      case 2: new_window(); break;
      case 3: open_file(); break;
      case 4: find_open(); break;
      case 5: zoom(1); break;
      case 6: zoom(-1); break;
      case 7: zoom(0); break;
      case 8: toggle_bookmark(); break;
      case 9: show_bookmarks(this); break;
      case 10: show_history(this); break;
      case 11: show_downloads(); break;
      case 12: show_settings(); break;
      case 13: fld::about_dialog("Web", "fl-web", "A web browser built with FLTK and WPE WebKit."); break;
      case 14: win->hide(); break;
      case 15: for (auto *w : std::vector<Browser *>(g_windows)) w->win->hide(); break;
      default:
        if (id >= 1000 && id - 1000 < (long)g_bookmarks.size()) load(g_bookmarks[(size_t)(id - 1000)].url);
    }
  }

  void open_file() {
    Fl_Native_File_Chooser fc;
    fc.title("Open File");
    fc.type(Fl_Native_File_Chooser::BROWSE_FILE);
    fc.filter("Web pages\t*.{html,htm,xhtml,svg,pdf,txt}\nAll files\t*");
    if (fc.show() == 0 && fc.filename()) load("file://" + fld::uri_encode_path(fc.filename()));
  }

  // keyboard shortcuts (also while the page has the focus)
  bool shortcut(int k, int s) {
    bool ctrl = s & FL_CTRL, shift = s & FL_SHIFT, alt = s & FL_ALT;
    if (ctrl && !alt) {
      switch (k) {
        case 't': new_tab("", true); return true;
        case 'n': new_window(); return true;
        case 'w': if (shift) win->hide(); else close_tab(cur); return true;
        case 'q': for (auto *w : std::vector<Browser *>(g_windows)) w->win->hide(); return true;
        case 'l': url->take_focus(); return true;
        case 'k': url->value("");
                  url->take_focus(); return true;
        case 'r': reload_or_stop(shift); return true;
        case 'f': find_open(); return true;
        case 'g': find_next(!shift); return true;
        case 'd': toggle_bookmark(); return true;
        case 'h': show_history(this); return true;
        case 'j': show_downloads(); return true;
        case 'o': if (shift) show_bookmarks(this); else open_file(); return true;
        case '=': case '+': case FL_KP + '+': zoom(1); return true;
        case '-': case FL_KP + '-': zoom(-1); return true;
        case '0': case FL_KP + '0': zoom(0); return true;
        case FL_Tab: if (!tabs.empty()) select((cur + (shift ? -1 : 1) + (int)tabs.size()) % (int)tabs.size()); return true;
        case FL_Page_Down: if (!tabs.empty()) select((cur + 1) % (int)tabs.size()); return true;
        case FL_Page_Up: if (!tabs.empty()) select((cur - 1 + (int)tabs.size()) % (int)tabs.size()); return true;
      }
      if (k >= '1' && k <= '9') {
        select(k == '9' ? (int)tabs.size() - 1 : k - '1');
        return true;
      }
      return false;
    }
    if (alt && !ctrl) {
      if (k == FL_Left) { go(-1); return true; }
      if (k == FL_Right) { go(1); return true; }
      if (k == FL_Home) { load(g_cfg.home); return true; }
      if (k == 'd') { url->take_focus(); return true; }
      return false;
    }
    switch (k) {
      case FL_F + 5: reload_or_stop(ctrl); return true;
      case FL_F + 6: url->take_focus(); return true;
      case FL_F + 3: find_next(!shift); return true;
      case FL_F + 11: set_fullscreen(!fullscreen); return true;
      case FL_Escape:
        if (findbar->visible()) { find_close(); return true; }
        if (tab() && tab()->loading) { reload_or_stop(); return true; }
        return false;
    }
    return false;
  }

  void set_fullscreen(bool on) {
    if (on == fullscreen) return;
    fullscreen = on;
    if (on) {
      tabbar->hide();
      bar->hide();
      progress->hide();
      win->fullscreen();
    } else {
      tabbar->show();
      bar->show();
      progress->show();
      win->fullscreen_off();
    }
    layout();
  }

  // ---- WebKit signals
  void connect(Tab *t);
  void context_menu(Tab *t, const std::string &link, const std::string &image, bool editable, bool selection);
};

int BrowserWindow::handle(int e) {
  if (e == FL_SHORTCUT && b->shortcut(Fl::event_key(), Fl::event_state())) return 1;
  int r = Fl_Double_Window::handle(e);
  if (e == FL_KEYBOARD && !r && b->shortcut(Fl::event_key(), Fl::event_state())) return 1;
  return r;
}

static void save_session();

void BrowserWindow::hide() {
  if (g_windows.size() == 1) save_session();
  Fl_Double_Window::hide();
  // the window goes once its tabs are closed (later: we may be inside one of their callbacks)
  Fl::add_timeout(0, [](void *d) {
    Browser *b = (Browser *)d;
    while (!b->tabs.empty()) {
      Tab *t = b->tabs.back();
      g_signal_handlers_disconnect_by_data(t->wv->view(), t);
      b->tabs.pop_back();
      b->content->remove(t->wv);
      delete t->wv;
      delete t;
    }
    g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), b), g_windows.end());
    Fl::delete_widget(b->win);
    delete b;
  }, b);
}

// ---------------------------------------------------------------------------
// WebKit signal handlers

static std::string str(const char *s) { return s ? s : ""; }

// a signal handler from a lambda without captures (G_CALLBACK is a macro:
// commas in the lambda would split its argument)
template <class F> static GCallback CB(F f) { return reinterpret_cast<GCallback>(+f); }

// runs fn outside of GLib's dispatch (dialogs run nested FLTK loops)
static void later(std::function<void()> fn) {
  auto *f = new std::function<void()>(std::move(fn));
  Fl::add_timeout(0, [](void *d) {
    auto *fp = (std::function<void()> *)d;
    (*fp)();
    delete fp;
    glib_loop_poke();
  }, f);
}

static bool tab_alive(Tab *t) {
  for (auto *w : g_windows)
    if (std::find(w->tabs.begin(), w->tabs.end(), t) != w->tabs.end()) return true;
  return false;
}

void Browser::connect(Tab *t) {
  WebKitWebView *v = t->wv->view();
  auto chrome = +[](GObject *, GParamSpec *, gpointer d) {
    Tab *t = (Tab *)d;
    WebKitWebView *v = t->wv->view();
    t->title = str(webkit_web_view_get_title(v));
    t->uri = str(webkit_web_view_get_uri(v));
    t->progress = webkit_web_view_get_estimated_load_progress(v);
    t->loading = webkit_web_view_is_loading(v);
    if (t->win->tab() == t) t->win->update_chrome();
    else t->win->tabbar->redraw();
  };
  for (const char *p : {"notify::title", "notify::uri", "notify::estimated-load-progress", "notify::is-loading"})
    g_signal_connect(v, p, G_CALLBACK(chrome), t);

  g_signal_connect(v, "load-changed", CB([](WebKitWebView *v, WebKitLoadEvent ev, gpointer d) {
    Tab *t = (Tab *)d;
    if (t->win->tab() == t) t->win->update_chrome();
    if (ev == WEBKIT_LOAD_FINISHED) {
      std::string uri = str(webkit_web_view_get_uri(v));
      if (fld::starts_with(uri, "http://") || fld::starts_with(uri, "https://") || fld::starts_with(uri, "file://")) {
        HistoryEntry e{time(nullptr), uri, str(webkit_web_view_get_title(v))};
        g_history.push_back(e);
        add_history(history_path(), e);
      }
    }
  }), t);
  // the title often arrives after the load finished: keep the history entry's title current
  g_signal_connect(v, "notify::title", CB([](GObject *o, GParamSpec *, gpointer) {
    WebKitWebView *v = WEBKIT_WEB_VIEW(o);
    std::string uri = str(webkit_web_view_get_uri(v)), title = str(webkit_web_view_get_title(v));
    if (!g_history.empty() && g_history.back().url == uri && g_history.back().title.empty() && !title.empty())
      g_history.back().title = title;
  }), t);

  g_signal_connect(v, "mouse-target-changed", CB([](WebKitWebView *, WebKitHitTestResult *hit, guint, gpointer d) {
    Tab *t = (Tab *)d;
    std::string link = webkit_hit_test_result_context_is_link(hit) ? str(webkit_hit_test_result_get_link_uri(hit)) : "";
    if (link != t->hover && t->win->tab() == t) t->win->set_status(link);
    t->hover = link;
  }), t);

  // window.open and target=_blank: a new tab next to this one
  g_signal_connect(v, "create", CB([](WebKitWebView *v, WebKitNavigationAction *, gpointer d) -> WebKitWebView * {
    Tab *t = (Tab *)d;
    Browser *b = t->win;
    int at = (int)(std::find(b->tabs.begin(), b->tabs.end(), t) - b->tabs.begin()) + 1;
    Tab *nt = b->new_tab("", false, v, at);
    g_signal_connect(nt->wv->view(), "ready-to-show", CB([](WebKitWebView *, gpointer d) {
      Tab *nt = (Tab *)d;
      Browser *b = nt->win;
      int i = (int)(std::find(b->tabs.begin(), b->tabs.end(), nt) - b->tabs.begin());
      if (i < (int)b->tabs.size()) b->select(i);
    }), nt);
    return WEBKIT_WEB_VIEW(g_object_ref(nt->wv->view()));
  }), t);
  g_signal_connect(v, "close", CB([](WebKitWebView *, gpointer d) {
    Tab *t = (Tab *)d;
    later([t] {
      if (!tab_alive(t)) return;
      Browser *b = t->win;
      b->close_tab((int)(std::find(b->tabs.begin(), b->tabs.end(), t) - b->tabs.begin()));
    });
  }), t);

  g_signal_connect(v, "decide-policy", CB([](WebKitWebView *, WebKitPolicyDecision *dec, WebKitPolicyDecisionType type, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
      WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(dec);
      if (!webkit_response_policy_decision_is_mime_type_supported(r)) {
        webkit_policy_decision_download(dec);
        return TRUE;
      }
      return FALSE;
    }
    WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(dec));
    std::string uri = str(webkit_uri_request_get_uri(webkit_navigation_action_get_request(a)));
    // other schemes (mailto:, magnet:, ...) go to their programs
    std::string scheme = fld::lower(uri.substr(0, uri.find(':')));
    static const char *ours[] = {"http", "https", "file", "about", "data", "blob", "javascript", "webkit"};
    if (uri.find(':') != std::string::npos && std::none_of(std::begin(ours), std::end(ours), [&](const char *s) { return scheme == s; })) {
      fld::spawn({"xdg-open", uri});
      webkit_policy_decision_ignore(dec);
      return TRUE;
    }
    // middle click or Ctrl+click on a link: a background tab
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
        (webkit_navigation_action_get_mouse_button(a) == 2 || (webkit_navigation_action_get_modifiers(a) & 1u /* Ctrl, in both input APIs */))) {
      Browser *b = t->win;
      int at = (int)(std::find(b->tabs.begin(), b->tabs.end(), t) - b->tabs.begin()) + 1;
      later([b, uri, at] {
        if (std::find(g_windows.begin(), g_windows.end(), b) != g_windows.end()) b->new_tab(uri, false, nullptr, at);
      });
      webkit_policy_decision_ignore(dec);
      return TRUE;
    }
    return FALSE;
  }), t);

  g_signal_connect(v, "script-dialog", CB([](WebKitWebView *, WebKitScriptDialog *dlg, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    webkit_script_dialog_ref(dlg);
    later([t, dlg] {
      std::string msg = fld::escape_at(str(webkit_script_dialog_get_message(dlg)));
      std::string host = url_host(t->uri);
      fl_message_title(host.empty() ? "Web" : host.c_str());
      switch (webkit_script_dialog_get_dialog_type(dlg)) {
        case WEBKIT_SCRIPT_DIALOG_ALERT: fl_message("%s", msg.c_str()); break;
        case WEBKIT_SCRIPT_DIALOG_CONFIRM:
          webkit_script_dialog_confirm_set_confirmed(dlg, fl_choice("%s", "Cancel", "OK", nullptr, msg.c_str()) == 1);
          break;
        case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
          webkit_script_dialog_confirm_set_confirmed(
              dlg, fl_choice("This page asks you to confirm that you want to leave. Changes you made may not be saved.",
                             "Stay", "Leave", nullptr) == 1);
          break;
        case WEBKIT_SCRIPT_DIALOG_PROMPT: {
          const char *r = fl_input("%s", str(webkit_script_dialog_prompt_get_default_text(dlg)).c_str(), msg.c_str());
          if (r) webkit_script_dialog_prompt_set_text(dlg, r);
          break;
        }
      }
      webkit_script_dialog_close(dlg);
      webkit_script_dialog_unref(dlg);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "run-file-chooser", CB([](WebKitWebView *, WebKitFileChooserRequest *req, gpointer) -> gboolean {
    g_object_ref(req);
    later([req] {
      Fl_Native_File_Chooser fc;
      bool multi = webkit_file_chooser_request_get_select_multiple(req);
      fc.type(multi ? Fl_Native_File_Chooser::BROWSE_MULTI_FILE : Fl_Native_File_Chooser::BROWSE_FILE);
      fc.title(multi ? "Choose Files" : "Choose a File");
      if (fc.show() == 0 && fc.count() > 0) {
        std::vector<std::string> files;
        for (int i = 0; i < fc.count(); i++) files.push_back(fc.filename(i));
        std::vector<const char *> ptrs;
        for (auto &f : files) ptrs.push_back(f.c_str());
        ptrs.push_back(nullptr);
        webkit_file_chooser_request_select_files(req, ptrs.data());
      } else {
        webkit_file_chooser_request_cancel(req);
      }
      g_object_unref(req);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "authenticate", CB([](WebKitWebView *, WebKitAuthenticationRequest *req, gpointer) -> gboolean {
    g_object_ref(req);
    later([req] {
      Fl_Double_Window dlg(380, 190, "Sign In");
      std::string text = fld::escape_at("The site " + str(webkit_authentication_request_get_host(req)) +
                                        " asks for a user name and password" +
                                        (webkit_authentication_request_get_realm(req) && *webkit_authentication_request_get_realm(req)
                                             ? std::string(" (\"") + webkit_authentication_request_get_realm(req) + "\")."
                                             : std::string(".")));
      Fl_Box msg(12, 10, 356, 44, text.c_str());
      msg.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
      Fl_Input user(110, 62, 258, 28, "User name:");
      Fl_Secret_Input pass(110, 96, 258, 28, "Password:");
      Fl_Button cancel(176, 148, 90, 30, "Cancel");
      Fl_Return_Button ok(278, 148, 90, 30, "Sign In");
      int result = -1;
      cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 0; }, &result);
      ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
      dlg.end();
      dlg.set_modal();
      dlg.show();
      while (result < 0 && dlg.shown()) Fl::wait();
      if (result == 1) {
        WebKitCredential *c = webkit_credential_new(user.value(), pass.value(), WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION);
        webkit_authentication_request_authenticate(req, c);
        webkit_credential_free(c);
      } else {
        webkit_authentication_request_cancel(req);
      }
      g_object_unref(req);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "load-failed-with-tls-errors", CB([](WebKitWebView *, char *uri, GTlsCertificate *cert, GTlsCertificateFlags, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    g_object_ref(cert);
    std::string u = str(uri);
    later([t, u, cert] {
      std::string host = url_host(u);
      int c = fl_choice("The connection to %s is not secure: its certificate is not trusted.\n\n"
                        "Someone may be trying to read or change what you send and receive.",
                        "Go Back", "Continue Anyway", nullptr, fld::escape_at(host).c_str());
      if (c == 1 && tab_alive(t)) {
        webkit_network_session_allow_tls_certificate_for_host(g_session, cert, host.c_str());
        webkit_web_view_load_uri(t->wv->view(), u.c_str());
      } else if (tab_alive(t) && webkit_web_view_can_go_back(t->wv->view())) {
        webkit_web_view_go_back(t->wv->view());
      }
      g_object_unref(cert);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "permission-request", CB([](WebKitWebView *, WebKitPermissionRequest *req, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    const char *what = WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req) ? "know your location"
                       : WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req) ? "show notifications"
                                                                        : nullptr;
    if (!what) {
      webkit_permission_request_deny(req);
      return TRUE;
    }
    g_object_ref(req);
    std::string host = url_host(t->uri), w = what;
    later([req, host, w] {
      if (fl_choice("%s wants to %s.", "Block", "Allow", nullptr, fld::escape_at(host).c_str(), w.c_str()) == 1)
        webkit_permission_request_allow(req);
      else webkit_permission_request_deny(req);
      g_object_unref(req);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "show-notification", CB([](WebKitWebView *, WebKitNotification *n, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    fld::Notification note;
    note.app_name = "Web";
    note.summary = str(webkit_notification_get_title(n));
    note.body = str(webkit_notification_get_body(n));
    note.icon = "fl-web";
    note.actions = {{"default", "Open"}};
    g_object_ref(n);
    Browser *b = t->win;
    fld::notify(note, [n, t, b](const std::string &) {
      if (tab_alive(t)) {
        b->win->show();
        b->select((int)(std::find(b->tabs.begin(), b->tabs.end(), t) - b->tabs.begin()));
        webkit_notification_clicked(n);
      }
      g_object_unref(n);
      glib_loop_poke();
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "context-menu", CB([](WebKitWebView *, WebKitContextMenu *, WebKitHitTestResult *hit, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    std::string link = webkit_hit_test_result_context_is_link(hit) ? str(webkit_hit_test_result_get_link_uri(hit)) : "";
    std::string image = webkit_hit_test_result_context_is_image(hit) ? str(webkit_hit_test_result_get_image_uri(hit)) : "";
    if (image.empty() && webkit_hit_test_result_context_is_media(hit)) image = str(webkit_hit_test_result_get_media_uri(hit));
    bool editable = webkit_hit_test_result_context_is_editable(hit);
    bool selection = webkit_hit_test_result_context_is_selection(hit);
    later([t, link, image, editable, selection] {
      if (tab_alive(t)) t->win->context_menu(t, link, image, editable, selection);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "enter-fullscreen", CB([](WebKitWebView *, gpointer d) -> gboolean {
    ((Tab *)d)->win->set_fullscreen(true);
    return FALSE;
  }), t);
  g_signal_connect(v, "leave-fullscreen", CB([](WebKitWebView *, gpointer d) -> gboolean {
    ((Tab *)d)->win->set_fullscreen(false);
    return FALSE;
  }), t);

  g_signal_connect(v, "web-process-terminated", CB([](WebKitWebView *, WebKitWebProcessTerminationReason r, gpointer d) {
    Tab *t = (Tab *)d;
    if (r == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) return;
    later([t, r] {
      if (!tab_alive(t)) return;
      const char *why = r == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "used too much memory" : "crashed";
      if (fl_choice("The page %s.", "Close", "Reload", nullptr, why) == 1) webkit_web_view_reload(t->wv->view());
    });
  }), t);

  WebKitFindController *fc = webkit_web_view_get_find_controller(v);
  g_signal_connect(fc, "found-text", CB([](WebKitFindController *, guint n, gpointer d) {
    Tab *t = (Tab *)d;
    Browser *b = t->win;
    if (b->tab() != t) return;
    b->find_label = n >= 1000 ? "1000+ matches" : std::to_string(n) + (n == 1 ? " match" : " matches");
    b->find_count->label(b->find_label.c_str());
    b->find_input->color(fld::palette().bg2);
    b->findbar->redraw();
  }), t);
  g_signal_connect(fc, "failed-to-find-text", CB([](WebKitFindController *, gpointer d) {
    Tab *t = (Tab *)d;
    Browser *b = t->win;
    if (b->tab() != t) return;
    b->find_count->label("Not found");
    b->find_input->color(fl_color_average(FL_RED, fld::palette().bg2, 0.25f));
    b->findbar->redraw();
  }), t);
}

void Browser::context_menu(Tab *t, const std::string &link, const std::string &image, bool editable, bool selection) {
  WebKitWebView *v = t->wv->view();
  std::vector<Fl_Menu_Item> m;
  auto item = [&](const char *l, long id, int flags = 0) { m.push_back({l, 0, nullptr, (void *)id, flags}); };
  if (!link.empty()) {
    item("Open Link in New Tab", 1);
    item("Open Link in New Window", 2);
    item("Copy Link Address", 3);
    item("Download Link", 4, FL_MENU_DIVIDER);
  }
  if (!image.empty()) {
    item("Open Image in New Tab", 5);
    item("Copy Image Address", 6);
    item("Save Image", 7, FL_MENU_DIVIDER);
  }
  if (editable) {
    item("Cut", 10);
    item("Copy", 11);
    item("Paste", 12);
    item("Select All", 13, FL_MENU_DIVIDER);
  } else if (selection) {
    item("Copy", 11, FL_MENU_DIVIDER);
  }
  if (link.empty() && image.empty() && !editable) {
    item("Back", 20, webkit_web_view_can_go_back(v) ? 0 : FL_MENU_INACTIVE);
    item("Forward", 21, webkit_web_view_can_go_forward(v) ? 0 : FL_MENU_INACTIVE);
    item("Reload", 22, FL_MENU_DIVIDER);
    item("Select All", 13);
    item(is_bookmarked(t->uri) ? "Remove Bookmark" : "Bookmark This Page", 23);
  }
  m.push_back({nullptr});
  const Fl_Menu_Item *r = m[0].popup(Fl::event_x(), Fl::event_y());
  if (!r) return;
  switch ((long)r->user_data()) {
    case 1: new_tab(link, false, nullptr, cur + 1); break;
    case 2: new_window({link}); break;
    case 3: Fl::copy(link.c_str(), (int)link.size(), 1); break;
    case 4: webkit_web_view_download_uri(v, link.c_str()); break;
    case 5: new_tab(image, false, nullptr, cur + 1); break;
    case 6: Fl::copy(image.c_str(), (int)image.size(), 1); break;
    case 7: webkit_web_view_download_uri(v, image.c_str()); break;
    case 10: webkit_web_view_execute_editing_command(v, WEBKIT_EDITING_COMMAND_CUT); break;
    case 11: webkit_web_view_execute_editing_command(v, WEBKIT_EDITING_COMMAND_COPY); break;
    case 12: webkit_web_view_execute_editing_command(v, WEBKIT_EDITING_COMMAND_PASTE); break;
    case 13: webkit_web_view_execute_editing_command(v, WEBKIT_EDITING_COMMAND_SELECT_ALL); break;
    case 20: go(-1); break;
    case 21: go(1); break;
    case 22: reload_or_stop(); break;
    case 23: toggle_bookmark(); break;
  }
  glib_loop_poke();
}

// ---------------------------------------------------------------------------
// Downloads

static Fl_Double_Window *g_dl_win;
static Fl_Hold_Browser *g_dl_list;

static std::string download_line(const Download &d) {
  std::string state;
  if (d.failed) state = "Failed: " + d.error;
  else if (d.done) state = "Done";
  else {
    guint64 got = webkit_download_get_received_data_length(d.dl);
    guint64 total = 0;
    if (WebKitURIResponse *r = webkit_download_get_response(d.dl)) total = webkit_uri_response_get_content_length(r);
    state = fld::human_size(got) + (total ? " of " + fld::human_size(total) : "");
  }
  return fld::escape_at(d.name) + "\t" + fld::escape_at(state);
}

static void refresh_downloads(void * = nullptr) {
  if (!g_dl_win || !g_dl_win->shown()) return;
  int sel = g_dl_list->value();
  g_dl_list->clear();
  for (auto it = g_downloads.rbegin(); it != g_downloads.rend(); ++it) g_dl_list->add(download_line(**it).c_str());
  if (sel > 0 && sel <= g_dl_list->size()) g_dl_list->value(sel);
  bool running = std::any_of(g_downloads.begin(), g_downloads.end(), [](auto &d) { return !d->done && !d->failed; });
  if (running) Fl::repeat_timeout(0.5, refresh_downloads);
}

static Download *selected_download() {
  int line = g_dl_list ? g_dl_list->value() : 0;
  if (line <= 0 || line > (int)g_downloads.size()) return nullptr;
  return g_downloads[g_downloads.size() - (size_t)line].get();
}

static void show_downloads() {
  if (!g_dl_win) {
    g_dl_win = new Fl_Double_Window(560, 340, "Downloads");
    g_dl_list = new Fl_Hold_Browser(10, 10, 540, 280);
    static int cols[] = {360, 0};
    g_dl_list->column_widths(cols);
    g_dl_list->column_char('\t');
    g_dl_list->callback([](Fl_Widget *, void *) {
      if (Fl::event_clicks())
        if (Download *d = selected_download())
          if (d->done) fld::open_path(d->path);
    });
    Fl_Button *open = new Fl_Button(10, 300, 100, 30, "Open");
    open->callback([](Fl_Widget *, void *) {
      if (Download *d = selected_download()) if (d->done) fld::open_path(d->path);
    });
    Fl_Button *folder = new Fl_Button(116, 300, 120, 30, "Show in Folder");
    folder->callback([](Fl_Widget *, void *) {
      Download *d = selected_download();
      fld::spawn({"fl-files", d ? fld::dirname(d->path) : fld::user_dir("DOWNLOAD")});
    });
    Fl_Button *cancel = new Fl_Button(242, 300, 100, 30, "Cancel");
    cancel->callback([](Fl_Widget *, void *) {
      if (Download *d = selected_download()) if (!d->done && !d->failed) webkit_download_cancel(d->dl);
      glib_loop_poke();
    });
    Fl_Button *clear = new Fl_Button(450, 300, 100, 30, "Clear List");
    clear->callback([](Fl_Widget *, void *) {
      g_downloads.erase(std::remove_if(g_downloads.begin(), g_downloads.end(), [](auto &d) {
        if (d->done || d->failed) { g_object_unref(d->dl); return true; }
        return false;
      }), g_downloads.end());
      refresh_downloads();
    });
    g_dl_win->resizable(g_dl_list);
    g_dl_win->end();
  }
  g_dl_win->show();
  Fl::remove_timeout(refresh_downloads);
  Fl::add_timeout(0, refresh_downloads);
}

static void download_started(WebKitNetworkSession *, WebKitDownload *dl, gpointer) {
  auto d = std::make_unique<Download>();
  d->dl = WEBKIT_DOWNLOAD(g_object_ref(dl));
  Download *dp = d.get();
  g_downloads.push_back(std::move(d));
  g_signal_connect(dl, "decide-destination", CB([](WebKitDownload *dl, char *suggested, gpointer d) -> gboolean {
    Download *dp = (Download *)d;
    std::string name = fld::basename(str(suggested));
    if (name.empty() || name == "/" || name == "." || name == "..") name = "download";
    std::string dir = fld::user_dir("DOWNLOAD");
    if (dir.empty()) dir = fld::home_dir();
    fld::mkdir_p(dir);
    dp->path = fld::unique_name(dir, name);
    dp->name = fld::basename(dp->path);
    webkit_download_set_destination(dl, dp->path.c_str());
    return TRUE;
  }), dp);
  g_signal_connect(dl, "finished", CB([](WebKitDownload *, gpointer d) {
    Download *dp = (Download *)d;
    if (dp->failed) return;
    dp->done = true;
    fld::Notification n;
    n.app_name = "Web";
    n.summary = "Download finished";
    n.body = dp->name;
    n.icon = "folder-download";
    n.category = "transfer.complete";
    n.actions = {{"default", "Open"}, {"folder", "Show in Folder"}};
    std::string path = dp->path;
    fld::notify(n, [path](const std::string &a) {
      if (a == "folder") fld::spawn({"fl-files", fld::dirname(path)});
      else fld::open_path(path);
    });
    refresh_downloads();
  }), dp);
  g_signal_connect(dl, "failed", CB([](WebKitDownload *, GError *e, gpointer d) {
    Download *dp = (Download *)d;
    dp->failed = true;
    dp->error = e && e->code == WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER ? "cancelled" : e ? e->message : "?";
    refresh_downloads();
  }), dp);
  later([] { show_downloads(); });
}

// ---------------------------------------------------------------------------
// History, bookmarks and settings dialogs

static void list_dialog(const char *title, Browser *b, bool history) {
  struct Ctx {
    Browser *b;
    bool history;
    Fl_Input *filter;
    Fl_Hold_Browser *list;
    std::vector<std::string> urls;
    void fill() {
      list->clear();
      urls.clear();
      if (history) {
        for (auto &e : history_view(g_history, filter->value(), 1000)) {
          char when[32];
          strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&e.time));
          list->add((fld::escape_at(e.title.empty() ? fallback_title(e.url) : e.title) + "\t" + fld::escape_at(e.url) + "\t" + when).c_str());
          urls.push_back(e.url);
        }
      } else {
        std::string f = fld::lower(filter->value());
        for (auto &bm : g_bookmarks) {
          if (!f.empty() && fld::lower(bm.url + " " + bm.title).find(f) == std::string::npos) continue;
          list->add((fld::escape_at(bm.title.empty() ? fallback_title(bm.url) : bm.title) + "\t" + fld::escape_at(bm.url)).c_str());
          urls.push_back(bm.url);
        }
      }
    }
    std::string selected() const {
      int l = list->value();
      return l > 0 && l <= (int)urls.size() ? urls[(size_t)l - 1] : "";
    }
  };
  Fl_Double_Window *w = new Fl_Double_Window(720, 460, title);
  Ctx *c = new Ctx{b, history, nullptr, nullptr, {}};
  c->filter = new Fl_Input(10, 10, 700, 28);
  c->filter->tooltip("Search");
  c->filter->when(FL_WHEN_CHANGED);
  c->filter->callback([](Fl_Widget *, void *d) { ((Ctx *)d)->fill(); }, c);
  c->list = new Fl_Hold_Browser(10, 46, 700, 364);
  static int hcols[] = {300, 300, 0}, bcols[] = {300, 0};
  c->list->column_widths(history ? hcols : bcols);
  c->list->column_char('\t');
  c->list->callback([](Fl_Widget *, void *d) {
    Ctx *c = (Ctx *)d;
    if (Fl::event_clicks() && !c->selected().empty() && std::find(g_windows.begin(), g_windows.end(), c->b) != g_windows.end())
      c->b->load(c->selected());
  }, c);
  Fl_Button *open = new Fl_Button(10, 420, 100, 30, "Open");
  open->callback([](Fl_Widget *, void *d) {
    Ctx *c = (Ctx *)d;
    if (!c->selected().empty() && std::find(g_windows.begin(), g_windows.end(), c->b) != g_windows.end()) c->b->load(c->selected());
  }, c);
  Fl_Button *tab = new Fl_Button(116, 420, 130, 30, "Open in New Tab");
  tab->callback([](Fl_Widget *, void *d) {
    Ctx *c = (Ctx *)d;
    if (!c->selected().empty() && std::find(g_windows.begin(), g_windows.end(), c->b) != g_windows.end())
      c->b->new_tab(c->selected(), true);
  }, c);
  Fl_Button *del = new Fl_Button(252, 420, 100, 30, "Remove");
  del->callback([](Fl_Widget *, void *d) {
    Ctx *c = (Ctx *)d;
    std::string u = c->selected();
    if (u.empty()) return;
    if (c->history) {
      g_history.erase(std::remove_if(g_history.begin(), g_history.end(), [&](const HistoryEntry &e) { return e.url == u; }), g_history.end());
      std::string data;
      for (auto &e : g_history) data += std::to_string((long long)e.time) + "\t" + one_line(e.url) + "\t" + one_line(e.title) + "\n";
      fld::write_file(history_path(), data);
    } else {
      g_bookmarks.erase(std::remove_if(g_bookmarks.begin(), g_bookmarks.end(), [&](const Bookmark &b) { return b.url == u; }), g_bookmarks.end());
      save_bookmarks(bookmarks_path(), g_bookmarks);
      for (auto *w : g_windows) w->update_chrome();
    }
    c->fill();
  }, c);
  if (history) {
    Fl_Button *clear = new Fl_Button(610, 420, 100, 30, "Clear All");
    clear->callback([](Fl_Widget *, void *d) {
      if (fl_choice("Clear the whole browsing history?", "Cancel", "Clear", nullptr) != 1) return;
      g_history.clear();
      fld::write_file(history_path(), "");
      ((Ctx *)d)->fill();
    }, c);
  } else {
    Fl_Button *rename = new Fl_Button(358, 420, 100, 30, "Rename…");
    rename->callback([](Fl_Widget *, void *d) {
      Ctx *c = (Ctx *)d;
      std::string u = c->selected();
      for (auto &bm : g_bookmarks)
        if (bm.url == u) {
          const char *n = fl_input("Name:", bm.title.c_str());
          if (n) { bm.title = n; save_bookmarks(bookmarks_path(), g_bookmarks); }
        }
      c->fill();
    }, c);
  }
  w->resizable(c->list);
  w->end();
  w->callback([](Fl_Widget *w, void *d) {
    w->hide();
    Fl::delete_widget(w);
    delete (Ctx *)d;
  }, c);
  c->fill();
  w->show();
}

static void show_history(Browser *b) { list_dialog("History", b, true); }
static void show_bookmarks(Browser *b) { list_dialog("Bookmarks", b, false); }

static void apply_settings() { webkit_settings_set_enable_javascript(g_settings, g_cfg.javascript); }

static void show_settings() {
  Fl_Double_Window dlg(480, 290, "Web Settings");
  Fl_Input home(140, 14, 326, 28, "Home page:");
  home.value(g_cfg.home.c_str());
  Fl_Choice search(140, 50, 200, 28, "Search with:");
  int sel = 0, i = 0;
  for (auto &e : search_engines()) {
    search.add(e.name);
    if (fld::lower(g_cfg.search) == fld::lower(e.name)) sel = i;
    i++;
  }
  search.value(sel);
  Fl_Check_Button js(140, 86, 300, 26, "Run JavaScript");
  js.value(g_cfg.javascript);
  Fl_Check_Button restore(140, 114, 300, 26, "Reopen the tabs of the last session");
  restore.value(g_cfg.restore);
  Fl_Box privacy(14, 150, 120, 28, "Privacy:");
  privacy.align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  Fl_Button clear(140, 150, 230, 28, "Clear Cookies and Site Data…");
  clear.callback([](Fl_Widget *, void *) {
    if (fl_choice("Remove all cookies, caches and data that web sites stored? You will be signed out of sites.",
                  "Cancel", "Clear", nullptr) != 1)
      return;
    webkit_website_data_manager_clear(webkit_network_session_get_website_data_manager(g_session),
                                      WEBKIT_WEBSITE_DATA_ALL, 0, nullptr, nullptr, nullptr);
    glib_loop_poke();
  });
  Fl_Button defb(140, 186, 230, 28, "Make Web the Default Browser");
  defb.callback([](Fl_Widget *, void *) {
    bool ok = fld::spawn({"xdg-settings", "set", "default-web-browser", "fl-web.desktop"});
    fl_message(ok ? "Web is now the default browser." : "Could not run xdg-settings.");
  });
  Fl_Button cancel(268, 248, 96, 30, "Cancel");
  Fl_Return_Button ok(370, 248, 96, 30, "Save");
  int result = -1;
  cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 0; }, &result);
  ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
  dlg.end();
  dlg.set_modal();
  dlg.show();
  while (result < 0 && dlg.shown()) Fl::wait();
  if (result != 1) return;
  std::string h = fld::trim(home.value());
  g_cfg.home = h.empty() ? "about:blank" : url_from_input(h, search_template(g_cfg.search));
  g_cfg.search = search_engines()[(size_t)std::max(0, search.value())].name;
  g_cfg.javascript = js.value();
  g_cfg.restore = restore.value();
  g_cfg.save();
  apply_settings();
}

// ---------------------------------------------------------------------------
// Windows and sessions

static Browser *new_window(const std::vector<std::string> &urls) {
  Browser *b = new Browser;
  if (urls.empty()) b->new_tab(g_cfg.home, true);
  for (auto &u : urls) b->new_tab(u, b->tabs.empty());
  b->win->show();
  b->layout();
  return b;
}

static void save_session() {
  std::string data;
  for (auto *w : g_windows)
    for (auto *t : w->tabs)
      if (!t->uri.empty()) data += one_line(t->uri) + "\n";
  fld::mkdir_p(data_dir());
  fld::write_file(session_path(), data);
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-web [URL or file...]\n");
      return 0;
    }
    if (a == "-v" || a == "--version") {
      printf("fl-web %s (WPE WebKit %u.%u.%u, %s)\n", FLD_VERSION, webkit_get_major_version(),
             webkit_get_minor_version(), webkit_get_micro_version(), backend_name());
      return 0;
    }
  }
  fld::init_app("fl-web", argc, argv);
  Fl::lock(); // notifications report back with Fl::awake
  std::vector<std::string> urls;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--new-window" || a.empty()) continue;
    if (fld::file_exists(a) && a.find("://") == std::string::npos) a = "file://" + fld::uri_encode_path(fld::abs_path(a));
    urls.push_back(url_from_input(a, ""));
  }
  g_cfg.load();
  fld::mkdir_p(data_dir());
  g_bookmarks = load_bookmarks(bookmarks_path());
  g_history = load_history(history_path());
  if (urls.empty() && g_cfg.restore) {
    std::string s;
    if (fld::read_file(session_path(), s))
      for (auto &l : fld::split(s, '\n'))
        if (!fld::trim(l).empty()) urls.push_back(fld::trim(l));
  }

  std::string cache = fld::cache_home() + "/fl-web";
  g_session = webkit_network_session_new(data_dir().c_str(), cache.c_str());
  webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(g_session),
                                               (data_dir() + "/cookies.sqlite").c_str(),
                                               WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  g_signal_connect(g_session, "download-started", G_CALLBACK(download_started), nullptr);
  g_settings = webkit_settings_new();
  webkit_settings_set_user_agent_with_application_details(g_settings, "fl-web", FLD_VERSION);
  webkit_settings_set_enable_developer_extras(g_settings, FALSE);
  apply_settings();
  g_ucm = webkit_user_content_manager_new();
  Backend::on_copy = [](const std::string &text) { Fl::copy(text.c_str(), (int)text.size(), 1); };

  glib_loop_start();
  new_window(urls);
  int r = Fl::run();
  save_session();
  return r;
}
