// fl-web: what its units share. main.cpp has the settings, the windows and
// sessions and main(); signals.cpp the WebKit signal handlers, the commands
// and the menu bar; logins.cpp reader mode and saved logins; downloads.cpp
// and dialogs.cpp (history, bookmarks, settings) the rest.
#pragma once
#include "fld/window_move.h"
#include "fld/json.h" // first: X11 headers define Bool
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
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Secret_Input.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <map>
#include <memory>
#include <string>
#include <cmath>
#include <sys/stat.h>
#include <vector>

#include "adblock.h"
#include "browser_util.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/mime.h"
#include "fld/notify.h"
#include "fld/tab_bar.h"
#include "fld/tab_move.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fld/window_size.h"
#include "glib_loop.h"
#include "gl_readback.h"
#include "webview.h"
#ifdef FLD_HAVE_READER
#include "reader.h"
#endif
#ifdef FLD_HAVE_SECRET
#include "secret.h"
#endif

using namespace web;


struct Config {
  std::string home = "https://duckduckgo.com/";
  std::string search = "DuckDuckGo";
  bool javascript = true;
  bool restore = false; // reopen the last session's tabs
  bool natural_scroll = false; // the page follows the fingers (wheel down scrolls up)
  bool devtools = false;       // developer tools (F12) through WebKit's inspector server (a restart to change)
  bool menubar = false;        // a menu bar above the tabs
  bool block_ads = true;       // content blocking with EasyList / EasyPrivacy
  bool easylist = true, easyprivacy = true;
  std::vector<std::string> adblock_extra; // more filter list addresses
  std::vector<std::string> adblock_allow; // sites where nothing is blocked
  bool save_passwords = true;              // offer to keep logins in the keyring
  std::vector<std::string> logins_never;   // sites (origins) not to offer it for
  int reader_font = 19;                    // reader mode's text size in px
  static std::vector<std::string> list(const std::string &v) {
    std::vector<std::string> out;
    for (auto &x : fld::split(v, ','))
      if (!fld::trim(x).empty()) out.push_back(fld::trim(x));
    return out;
  }
  static std::string join(const std::vector<std::string> &v) {
    std::string s;
    for (auto &x : v) s += (s.empty() ? "" : ",") + x;
    return s;
  }
  web::adblock::Settings adblock() const {
    web::adblock::Settings a;
    a.enabled = block_ads;
    a.easylist = easylist;
    a.easyprivacy = easyprivacy;
    a.extra = adblock_extra;
    a.allow = adblock_allow;
    return a;
  }
  void load() {
    fld::Ini c;
    c.load(path());
    home = c.get("Web", "HomePage", home);
    search = c.get("Web", "SearchEngine", search);
    javascript = c.get_bool("Web", "JavaScript", javascript);
    restore = c.get_bool("Web", "RestoreTabs", restore);
    natural_scroll = c.get_bool("Web", "NaturalScrolling", natural_scroll);
    devtools = c.get_bool("Web", "DeveloperTools", devtools);
    menubar = c.get_bool("Web", "ShowMenuBar", menubar);
    block_ads = c.get_bool("Web", "BlockAds", block_ads);
    std::string lists = c.get("Web", "AdblockLists", "easylist,easyprivacy");
    easylist = false;
    easyprivacy = false;
    for (auto &l : list(lists)) {
      if (l == "easylist") easylist = true;
      if (l == "easyprivacy") easyprivacy = true;
    }
    adblock_extra = list(c.get("Web", "AdblockExtra", ""));
    adblock_allow = list(c.get("Web", "AdblockAllow", ""));
    save_passwords = c.get_bool("Web", "SavePasswords", save_passwords);
    logins_never = list(c.get("Web", "LoginsNever", ""));
    reader_font = std::max(10, std::min(40, c.get_int("Web", "ReaderFontSize", reader_font)));
  }
  void save() const {
    fld::Ini c;
    c.load(path());
    c.set("Web", "HomePage", home);
    c.set("Web", "SearchEngine", search);
    c.set_bool("Web", "JavaScript", javascript);
    c.set_bool("Web", "RestoreTabs", restore);
    c.set_bool("Web", "NaturalScrolling", natural_scroll);
    c.set_bool("Web", "DeveloperTools", devtools);
    c.set_bool("Web", "ShowMenuBar", menubar);
    c.set_bool("Web", "BlockAds", block_ads);
    std::vector<std::string> lists;
    if (easylist) lists.push_back("easylist");
    if (easyprivacy) lists.push_back("easyprivacy");
    c.set("Web", "AdblockLists", join(lists));
    c.set("Web", "AdblockExtra", join(adblock_extra));
    c.set("Web", "AdblockAllow", join(adblock_allow));
    c.set_bool("Web", "SavePasswords", save_passwords);
    c.set("Web", "LoginsNever", join(logins_never));
    c.set_int("Web", "ReaderFontSize", reader_font);
    c.save(path());
  }
  static std::string path() { return fld::fld_config_dir() + "/web.ini"; }
};
struct Download {
  WebKitDownload *dl;
  std::string path, name, error;
  bool done = false, failed = false;
};
class Browser;

struct Tab;

// a captureless lambda as a GLib signal handler
template <class F> static GCallback CB(F f) { return reinterpret_cast<GCallback>(+f); }

extern Config g_cfg;  // main.cpp

// ---- defined in one unit, used in others
std::string bookmarks_path();
std::string history_path();
double site_zoom(const std::string &uri);
void remember_site_zoom(const std::string &uri, double z);
std::string str(const char *s);
extern WebKitNetworkSession *g_session;
void show_downloads();
extern int g_inspector_port;
extern WebKitSettings *g_settings;
extern WebKitUserContentManager *g_ucm;
extern std::vector<Bookmark> g_bookmarks;
void later(std::function<void()> fn);
extern std::vector<HistoryEntry> g_history;
extern std::vector<std::unique_ptr<Download>> g_downloads;
bool is_bookmarked(const std::string &url);
bool tab_alive(Tab *t);
extern std::vector<Browser *> g_windows;
WebKitNetworkSession *private_session();
Browser *new_window(const std::vector<std::string> &urls = {}, bool priv = false);
void login_submitted(const std::string &json);
std::vector<std::string> session_uris();
void download_started(WebKitNetworkSession *, WebKitDownload *dl, gpointer);
void save_session();
void show_history(Browser *b);
void show_bookmarks(Browser *b);
void apply_settings();
void show_settings();
void show_passwords();

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
  // the last hit test under the pointer, for the context menu
  std::string hit_link, hit_image;
  bool hit_editable = false, hit_selection = false;
  double menu_shown = 0; // when WebKit last asked for a context menu
  bool reader = false;   // the page shows in reader mode
  std::string reader_of; // the address it came from
  bool filled = false;   // a saved login was filled into this page
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
  fld::TabBar *tabbar;
  Fl_Menu_Bar *menubar;
  Fl_Box *progress;
  Fl_Group *content;
  Fl_Group *findbar;
  Fl_Input *find_input;
  Fl_Box *find_count;
  Fl_Box *status;
  Fl_Window *status_win; // a subwindow: kept above the page's own subsurface
  std::vector<Tab *> tabs;
  int cur = -1;
  bool fullscreen = false;
  bool priv = false;                     // a private window
  WebKitNetworkSession *session;         // g_session, or the private one
  std::vector<std::string> closed;       // addresses of closed tabs (Reopen Closed Tab)
  int chrome_y = 0;                      // top of the tab bar (below the menu bar)
  static const int TAB_H = 32, TB = 42, FIND_H = 38;
  static int menu_h() { return std::max(24, fld::ui_font_size() + 12); }

  explicit Browser(bool private_window = false) : priv(private_window) {
    session = priv ? private_session() : g_session;
    const fld::Palette &p = fld::palette();
    int sx, sy, sw, sh;
    Fl::screen_work_area(sx, sy, sw, sh);
    int W = std::min(1100, sw - 40), H = std::min(760, sh - 40);
    win = new BrowserWindow(W, H, this);
    menubar = make_menubar(W);
    tabbar = new fld::TabBar(0, 0, W, TAB_H);
    tabbar->empty_label = "New Tab";
    if (priv) tabbar->tint = fl_rgb_color(0x3b, 0x2a, 0x55);
    tabbar->on_select = [this](int i) { select(i); };
    tabbar->on_close = [this](int i) { later_close(i); };
    tabbar->on_new = [this] { new_tab("", true); };
    tabbar->on_move = [this](int from, int to) {
      fld::move_element(tabs, from, to);
      cur = fld::moved_index(cur, from, to);
      refresh_tabs();
    };
    tabbar->on_menu = [this](int i) { tab_menu(i); };
    tabbar->on_detach = [this](int i) { later_detach(i); };
    // tabs move to other windows: live within this program, by address to
    // another fl-web; private tabs only into private windows of this program
    tabbar->app_id = "fl-web";
    tabbar->on_export = [this](int i) { return export_tab(i); };
    tabbar->on_import = [this](const std::string &state, int at, bool same) { return import_tab(state, at, same); };
    tabbar->on_moved_away = [this](int i) {
      // a live move already took it out of this window; else close it here
      Tab *t = exported;
      exported = nullptr;
      if (t && index_of(t) == i) later_close(i);
    };
    tabbar->on_tear_off = [this](int i, const std::string &file) {
      fld::discard_tab_file(file);
      later_detach(i);
    };
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
    status_win = nullptr; // the link under the pointer (set_status)
    status = nullptr;
    win->resizable(content);
    win->end();
    win->size_range(400, 300);
    if (!priv) fld::remember_window_size(win, "fl-web");
    fld::set_window_icon(win, "fl-web");
    g_windows.push_back(this);
    place_chrome();
  }

  Tab *tab() const { return cur >= 0 && cur < (int)tabs.size() ? tabs[(size_t)cur] : nullptr; }
  WebKitWebView *view() const { return tab() ? tab()->wv->view() : nullptr; }

  // the menu bar, tab bar, toolbar and progress line, from the top
  void place_chrome() {
    int W = win->w();
    int y = 0;
    if (g_cfg.menubar && !fullscreen) {
      menubar->resize(0, 0, W, menu_h());
      menubar->show();
      y = menu_h();
    } else {
      menubar->hide();
    }
    chrome_y = y;
    tabbar->resize(0, y, W, TAB_H);
    bar->resize(0, y + TAB_H, W, TB);
    progress->resize(0, y + TAB_H + TB, progress->w(), 2);
  }
  int chrome_h() const { return fullscreen ? 0 : chrome_y + TAB_H + TB + 2; }

  void layout() {
    place_chrome();
    int W = win->w(), H = win->h();
    int top = chrome_h();
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
    t->wv = new WebView(content->x(), content->y(), content->w(), content->h(), related, g_settings, session, g_ucm);
    content->end();
    t->wv->hide();
    attach(t);
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
    refresh_tabs();
    glib_loop_poke();
    return t;
  }

  // hooks a tab's web view up to this window (new tabs, and tabs moved here)
  void attach(Tab *t) {
    t->win = this;
    t->wv->key_filter = [this](int k, int s) { return shortcut(k, s); };
    t->wv->on_right_click = [this, t](int x, int y) { right_clicked(t, x, y); };
  }

  void refresh_tabs() {
    std::vector<fld::TabInfo> v;
    for (auto *t : tabs) {
      fld::TabInfo i;
      i.label = t->title.empty() ? (t->uri.empty() ? std::string("New Tab") : fallback_title(t->uri)) : t->title;
      i.tooltip = t->uri.empty() ? i.label : i.label + "\n" + t->uri;
      i.busy = t->loading;
      v.push_back(i);
    }
    tabbar->set_tabs(v, std::max(0, cur));
  }

  int index_of(Tab *t) const {
    auto it = std::find(tabs.begin(), tabs.end(), t);
    return it == tabs.end() ? -1 : (int)(it - tabs.begin());
  }

  // closing from the tab bar's own event handler: later
  void later_close(int i) {
    if (i < 0 || i >= (int)tabs.size()) return;
    Tab *t = tabs[(size_t)i];
    Fl::add_timeout(0, [](void *d) {
      Tab *t = (Tab *)d;
      for (auto *w : g_windows) {
        int k = w->index_of(t);
        if (k >= 0) { w->close_tab(k); break; }
      }
    }, t);
  }
  void later_detach(int i) {
    if (i < 0 || i >= (int)tabs.size()) return;
    Tab *t = tabs[(size_t)i];
    Fl::add_timeout(0, [](void *d) {
      Tab *t = (Tab *)d;
      for (auto *w : std::vector<Browser *>(g_windows))
        if (w->index_of(t) >= 0) { w->tab_to_new_window(t); break; }
    }, t);
  }

  Tab *exported = nullptr; // the tab being dragged to another window
  std::string export_tab(int i) {
    if (i < 0 || i >= (int)tabs.size()) return "";
    Tab *t = tabs[(size_t)i];
    exported = t;
    fld::Json j = fld::Json::object();
    j.set("uri", t->uri);
    j.set("private", priv);
    j.set("tab", std::to_string((uintptr_t)t));
    return j.dump();
  }
  bool import_tab(const std::string &state, int at, bool same_process) {
    fld::Json j;
    if (!fld::Json::parse(state, j)) return false;
    if (j["private"].as_bool() != priv) return false; // private and normal tabs do not mix
    if (same_process) {
      // the live tab, page and history included
      uintptr_t id = (uintptr_t)strtoull(j["tab"].as_string().c_str(), nullptr, 10);
      for (auto *w : g_windows) {
        if (w == this) continue;
        for (int k = 0; k < (int)w->tabs.size(); k++) {
          Tab *t = w->tabs[(size_t)k];
          if ((uintptr_t)t != id) continue;
          w->detach(k);
          if (w->tabs.empty()) w->win->hide(); // its last tab left: the window goes
          adopt(t);
          int to = std::max(0, std::min((int)tabs.size() - 1, at));
          fld::move_element(tabs, (int)tabs.size() - 1, to);
          cur = -1;
          select(to);
          win->show();
          return true;
        }
      }
      return false;
    }
    if (priv) return false;
    std::string uri = j["uri"].as_string();
    new_tab(uri, true, nullptr, std::max(0, std::min((int)tabs.size(), at)));
    win->show();
    return true;
  }

  // moves a tab (its page stays loaded) into a new window
  void tab_to_new_window(Tab *t) {
    int i = index_of(t);
    if (i < 0 || tabs.size() < 2) return;
    detach(i);
    Browser *b = new Browser(priv);
    b->adopt(t);
    b->win->show();
    b->layout();
    b->select(0);
  }
  // takes the tab out of this window without destroying it
  void detach(int i) {
    Tab *t = tabs[(size_t)i];
    t->wv->activate_view(false);
    t->wv->hide();
    content->remove(t->wv);
    tabs.erase(tabs.begin() + i);
    if (cur > i || cur >= (int)tabs.size()) cur--;
    int c = std::max(0, cur);
    cur = -1;
    select(c);
  }
  void adopt(Tab *t) {
    content->add(t->wv);
    t->wv->resize(content->x(), content->y(), content->w(), content->h());
    attach(t);
    tabs.push_back(t);
    refresh_tabs();
  }

  void reopen_closed() {
    if (closed.empty()) return;
    std::string u = closed.back();
    closed.pop_back();
    new_tab(u, true, nullptr, cur + 1);
  }

  void right_clicked(Tab *t, int x, int y);
  void tab_menu(int i);
  Fl_Menu_Bar *make_menubar(int W);
  void command(long id);
  void build_menu(fld::Menu &m, bool for_bar);
  void save_page();
  void view_source();
  void open_devtools();
  void toggle_reader();
  void fill_login(bool choose);

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
    refresh_tabs();
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
    if (!t->uri.empty()) {
      closed.push_back(t->uri);
      if (closed.size() > 20) closed.erase(closed.begin());
    }
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
    // the site opens at this zoom next time (not remembered in private windows)
    const char *u = webkit_web_view_get_uri(view());
    if (!priv && u) remember_site_zoom(u, nz);
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
    progress->resize(0, chrome_y + TAB_H + TB, pw, 2);
    progress->parent()->redraw();
    std::string title = (t->title.empty() ? (t->uri.empty() ? std::string("New Tab") : fallback_title(t->uri)) : t->title) +
                        (priv ? " — Private — Web" : " — Web");
    win->copy_label(title.c_str());
    if (Fl::focus() != url) url->value(t->uri.c_str());
    bar->redraw();
    refresh_tabs();
  }
  std::string status_text;
  void set_status(const std::string &s, double secs = 0) {
    status_text = fld::escape_at(s);
    Fl::remove_timeout(hide_status, this);
    if (s.empty()) {
      remove_status();
      win->redraw();
      return;
    }
    fl_font(FL_HELVETICA, 12);
    int w = std::min(win->w() - 20, (int)fl_width(status_text.c_str()) + 16);
    int y = content->y() + content->h() - 22;
    // A subwindow over the page (pages may be subsurfaces of their own), made
    // at its size each time: FLTK does not redraw a resized subwindow at once
    if (!status_win || status_win->x() != 0 || status_win->y() != y || status_win->w() != w) {
      remove_status();
      win->begin();
      status_win = new Fl_Window(0, y, w, 22);
      status = new Fl_Box(0, 0, w, 22);
      status->box(FL_FLAT_BOX);
      status->color(fld::palette().bg2);
      status->labelsize(12);
      status->labelcolor(fld::palette().fg);
      status->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
      status_win->end();
      win->end();
      web::wl_present::set_overlay(win, status_win);
    }
    status->label(status_text.c_str());
    status->redraw();
    status_win->show();
    web::wl_present::pass_input(status_win); // the pointer goes on to the page
    web::wl_present::overlays_changed(win);
    win->redraw();
    if (secs > 0) Fl::add_timeout(secs, hide_status, this);
  }
  static void hide_status(void *d) { ((Browser *)d)->set_status(""); }
  void remove_status() {
    if (!status_win) return;
    web::wl_present::set_overlay(win, nullptr);
    status_win->hide();
    win->remove(status_win);
    Fl::delete_widget(status_win);
    status_win = nullptr;
    status = nullptr;
  }

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
    fld::Menu m;
    build_menu(m, false);
    m.popup(menu->x(), menu->y() + menu->h());
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
        case 't': if (shift) reopen_closed(); else new_tab("", true); return true;
        case 'n': new_window({}, shift || priv); return true;
        case 'm': if (shift) { command(19); return true; } return false;
        case 'u': view_source(); return true;
        case 's': save_page(); return true;
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
      case FL_F + 9: toggle_reader(); return true;
      case FL_F + 12: open_devtools(); return true;
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
    if (on) set_status("Press F11 to leave full screen", 2.5);
  }

  // ---- WebKit signals
  void connect(Tab *t);
  void context_menu(Tab *t, const std::string &link, const std::string &image, bool editable, bool selection, int x,
                    int y);
};
