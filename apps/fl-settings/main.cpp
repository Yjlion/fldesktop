// fl-settings: the fldesktop settings center
#include <FL/Fl.H>
#include <FL/Fl_Browser_.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scroll.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Text_Display.H>
#include <FL/Fl_Valuator.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fld/window_size.h"
#include "settings_page.h"
#include "settings_search.h"

static Fl_Double_Window *g_win;
static Fl_Input *g_search;
static Fl_Hold_Browser *g_list;
class ContentArea;
static ContentArea *g_content;
static std::map<std::string, Fl_Group *> g_created;
// the size a page needs: its widgets' extent, or (height) a minimum for pages
// whose list or tabs stretch to fill the window
static std::map<Fl_Group *, int> g_page_min_h, g_page_min_w;
static const int kMinPageH = 380; // pages are not laid out smaller
static const int kMinPageW = 530; // the narrowest the pages are made for
static std::string g_current;

struct Entry {
  bool header = false;
  std::string page_id;     // compiled-in page
  std::string desktop_id;  // external settings program
  std::string label;
  std::string focus;       // a setting on the page to point at (search results)
};
static std::vector<Entry> g_entries;
static std::vector<fld::DesktopEntry> g_external;

static const char *kCategories[] = {"Personal", "Hardware", "Network", "System"};

static bool matches(const std::string &text, const std::string &q) {
  return fld::lower(text).find(q) != std::string::npos;
}

static void rebuild_list() {
  std::string q = fld::lower(fld::trim(g_search->value()));
  g_entries.clear();
  g_list->clear();
  auto add_header = [](const std::string &t) {
    Entry e;
    e.header = true;
    e.label = t;
    g_entries.push_back(e);
    // 13 px bold in the dimmed text colour: readable, still set apart from the pages
    g_list->add(("@b@C" + std::to_string((unsigned)fld::palette().fg_dim) + fld::escape_at(t)).c_str());
  };
  for (const char *cat : kCategories) {
    bool any = false;
    for (auto &p : fld::all_pages()) {
      if (p.category != cat) continue;
      if (!q.empty() && !matches(p.title, q) && !matches(p.keywords, q) && !matches(p.id, q)) continue;
      if (!any) { add_header(cat); any = true; }
      Entry e;
      e.page_id = p.id;
      e.label = p.title;
      g_entries.push_back(e);
      g_list->add(("  " + fld::escape_at(p.title)).c_str());
      g_list->icon(g_list->size(), fld::ui_icon_first({p.icon, "preferences-system"}, 16));
    }
  }
  // single settings on all pages: "Tap to click — Mouse & Touchpad"
  if (!q.empty()) {
    bool first = true;
    for (auto &h : settings_search::search(q, settings_search::index(), 12)) {
      const fld::PageInfo *pi = nullptr;
      for (auto &p : fld::all_pages())
        if (p.id == h.page) pi = &p;
      if (!pi) continue; // a page this build does not have
      if (first) add_header("Settings");
      first = false;
      Entry e;
      e.page_id = h.page;
      e.focus = h.label;
      e.label = h.label;
      g_entries.push_back(e);
      g_list->add(("  " + fld::escape_at(h.label) + "  — " + fld::escape_at(pi->title)).c_str());
      g_list->icon(g_list->size(), fld::ui_icon_first({pi->icon, "preferences-system"}, 16));
    }
  }
  bool any = false;
  for (auto &d : g_external) {
    if (!q.empty() && !matches(d.name, q) && !matches(d.comment, q)) continue;
    if (!any) { add_header("More"); any = true; }
    Entry e;
    e.desktop_id = d.id;
    e.label = d.name;
    g_entries.push_back(e);
    g_list->add(("  " + fld::escape_at(d.name)).c_str());
    g_list->icon(g_list->size(), fld::ui_icon_first({d.icon, "preferences-system"}, 16));
  }
  for (size_t i = 0; i < g_entries.size(); i++)
    if (g_entries[i].page_id == g_current) g_list->value((int)i + 1);
  // selecting scrolls the line into view, which could hide the heading
  // above it ("Personal" above "Appearance"): start from the top unless
  // that would leave the selected line out of sight
  int sel = g_list->value();
  g_list->topline(1);
  if (sel > 0 && !g_list->displayed(sel)) g_list->middleline(sel);
}

// The page area scrolls: pages keep the height their widgets need and a
// scroll bar appears when the window is shorter (small screens).
class ContentArea : public Fl_Scroll {
public:
  ContentArea(int X, int Y, int W, int H) : Fl_Scroll(X, Y, W, H) { type(Fl_Scroll::BOTH); }
  // The wheel scrolls the page, unless it is over a list or another widget
  // that scrolls itself. (FLTK would hand it to any widget of the page that
  // takes it, wherever that is.)
  int handle(int e) override {
    if (e == FL_MOUSEWHEEL && Fl::event_dy() && scrollbar.visible()) {
      Fl_Widget *w = scrolls_itself(this);
      if (w && w->handle(FL_MOUSEWHEEL)) return 1;
      int top = std::max(0, std::min(yposition() + Fl::event_dy() * 48, (int)scrollbar.maximum()));
      scroll_to(xposition(), top);
      scrollbar.value(top);
      return 1;
    }
    return Fl_Scroll::handle(e);
  }
  // the widget under the mouse that scrolls on its own, if any
  Fl_Widget *scrolls_itself(Fl_Group *g) {
    for (int i = g->children(); i--;) {
      Fl_Widget *c = g->child(i);
      if (c == &scrollbar || c == &hscrollbar || !c->visible() || !Fl::event_inside(c)) continue;
      if (dynamic_cast<Fl_Browser_ *>(c) || dynamic_cast<Fl_Text_Display *>(c) || dynamic_cast<Fl_Valuator *>(c) ||
          dynamic_cast<Fl_Scroll *>(c) || dynamic_cast<Fl_Spinner *>(c))
        return c;
      if (Fl_Group *cg = c->as_group())
        if (Fl_Widget *w = scrolls_itself(cg)) return w;
    }
    return nullptr;
  }
  static int page_h(Fl_Group *g, int H) {
    auto it = g_page_min_h.find(g);
    return std::max(H, it == g_page_min_h.end() ? H : it->second);
  }
  static int min_w(Fl_Group *g) {
    auto it = g_page_min_w.find(g);
    return it == g_page_min_w.end() ? kMinPageW : it->second;
  }
  // places one page: at least the visible size and the page's own minimum;
  // scroll bars take their room when it is more
  void place(Fl_Group *g) {
    int sb = Fl::scrollbar_size();
    int ph = page_h(g, h());
    bool need_v = ph > h();
    bool need_h = min_w(g) > w() - (need_v ? sb : 0);
    if (need_h) {
      ph = page_h(g, h() - sb);
      need_v = ph > h() - sb;
    }
    int pw = std::max(w() - (need_v ? sb : 0), min_w(g));
    g->resize(x() - xposition(), y() - yposition(), pw, ph);
  }
  void resize(int X, int Y, int W, int H) override {
    int top = yposition(), left = xposition();
    // moves the pages along and places the scroll bars; the pages are sized here
    Fl_Scroll::resize(X, Y, W, H);
    int maxtop = 0, maxleft = 0;
    for (int i = 0; i < children(); i++) {
      Fl_Widget *c = child(i);
      if (c == &scrollbar || c == &hscrollbar) continue;
      place((Fl_Group *)c);
      if (c->visible()) {
        maxtop = std::max(0, c->h() - H);
        maxleft = std::max(0, c->w() - W);
      }
    }
    // keep the scroll position when it still fits
    scroll_to(std::min(left, maxleft), std::min(top, maxtop));
    redraw();
  }
};

// how far the widgets inside g reach (right and bottom), relative to x0, y0
static void content_extent(Fl_Group *g, int x0, int y0, int &right, int &bottom) {
  for (int i = 0; i < g->children(); i++) {
    Fl_Widget *c = g->child(i);
    if (!c->visible()) continue;
    // groups are as large as the page: what counts is inside them (a
    // scrolling group, the wallpaper gallery, holds more than it shows)
    Fl_Group *cg = c->as_group();
    if (cg && !dynamic_cast<Fl_Scroll *>(cg) && cg->children()) {
      content_extent(cg, x0, y0, right, bottom);
      continue;
    }
    right = std::max(right, c->x() + c->w() - x0);
    bottom = std::max(bottom, c->y() + c->h() - y0);
  }
}

// a page whose inner group stretches a widget (a list, tabs) fills the window
static bool fills_window(Fl_Group *g) {
  for (int i = 0; i < g->children(); i++)
    if (Fl_Group *cg = g->child(i)->as_group())
      if (cg->resizable() && cg->resizable() != cg) return true;
  return false;
}

static void show_page(const std::string &id) {
  const fld::PageInfo *info = nullptr;
  for (auto &p : fld::all_pages()) if (p.id == id) info = &p;
  if (!info) return;
  for (auto &kv : g_created) kv.second->hide();
  Fl_Group *g;
  auto it = g_created.find(id);
  g_content->scroll_to(0, 0);
  if (it == g_created.end()) {
    // laid out for the visible size (not less than the minimum), then
    // measured: form pages need their rows' extent, list pages stretch and
    // scroll only below the minimum
    int cw = std::max(kMinPageW, g_content->w() - Fl::scrollbar_size());
    g_content->begin();
    g = info->create(g_content->x(), g_content->y(), cw, std::max(kMinPageH, g_content->h()));
    g_content->end();
    g_created[id] = g;
    int right = 0, bottom = 0;
    content_extent(g, g->x(), g->y(), right, bottom);
    g_page_min_h[g] = fills_window(g) ? kMinPageH : std::max(kMinPageH, bottom + 16);
    g_page_min_w[g] = std::max(kMinPageW, std::min(right + 12, 1400));
  } else {
    g = it->second;
  }
  g->show();
  g_content->place(g);
  g_current = id;
  std::string title = info->title + " – Settings";
  g_win->copy_label(title.c_str());
  int line = g_list->value();
  bool on_result = line >= 1 && line <= (int)g_entries.size() && g_entries[(size_t)line - 1].page_id == id;
  if (!on_result)
    for (size_t i = 0; i < g_entries.size(); i++)
      if (g_entries[i].page_id == id && g_entries[i].focus.empty()) {
        g_list->value((int)i + 1);
        break;
      }
  g_win->redraw();
}

// ---- pointing at a setting found by the search ------------------------------------

static Fl_Widget *g_flash;
static Fl_Color g_flash_label;
static int g_flash_count;

static Fl_Widget *find_label(Fl_Group *g, const std::string &want) {
  for (int i = 0; i < g->children(); i++) {
    Fl_Widget *c = g->child(i);
    if (c->label() && settings_search::normalize(c->label()) == want) return c;
    if (Fl_Group *cg = c->as_group())
      if (Fl_Widget *w = find_label(cg, want)) return w;
  }
  return nullptr;
}

static void flash_tick(void *) {
  if (!g_flash) return;
  bool on = g_flash_count % 2 == 0;
  g_flash->labelcolor(on ? fld::palette().accent : g_flash_label);
  g_flash->redraw_label();
  g_flash->redraw();
  if (++g_flash_count < 6) Fl::repeat_timeout(0.3, flash_tick);
  else {
    g_flash->labelcolor(g_flash_label);
    g_flash->redraw_label();
    g_flash = nullptr;
  }
}

// scrolls the page to the setting with that label, gives it the focus and
// makes its label blink a few times
static void point_at(const std::string &page, const std::string &label, bool focus = true) {
  auto it = g_created.find(page);
  if (it == g_created.end()) return;
  Fl_Widget *w = find_label(it->second, settings_search::normalize(label));
  if (!w) return;
  if (g_flash) {
    Fl::remove_timeout(flash_tick);
    g_flash->labelcolor(g_flash_label);
    g_flash->redraw_label();
  }
  int top = w->y() - g_content->y() + g_content->yposition() - 40;
  if (top > 0 && w->y() + w->h() > g_content->y() + g_content->h()) {
    g_content->scroll_to(g_content->xposition(), std::min(top, (int)g_content->scrollbar.maximum()));
  }
  if (focus && w->visible_focus() && w->active_r()) w->take_focus();
  g_flash = w;
  g_flash_label = w->labelcolor();
  g_flash_count = 0;
  Fl::add_timeout(0.05, flash_tick);
}

static void list_cb(Fl_Widget *, void *) {
  int line = g_list->value();
  if (line < 1 || line > (int)g_entries.size()) return;
  const Entry &e = g_entries[line - 1];
  if (e.header) {
    // keep the current page selected
    rebuild_list();
    return;
  }
  if (!e.page_id.empty()) {
    show_page(e.page_id);
    if (!e.focus.empty()) point_at(e.page_id, e.focus);
  } else {
    for (auto &d : g_external)
      if (d.id == e.desktop_id) d.launch();
    rebuild_list();
  }
}

int main(int argc, char **argv) {
  std::string start;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-settings [PAGE]\n       fl-settings --list\n");
      return 0;
    }
    if (a == "--list") {
      for (auto &p : fld::all_pages()) printf("%-12s %s\n", p.id.c_str(), p.title.c_str());
      return 0;
    }
    if (a[0] != '-') start = a;
  }
  fld::init_app("fl-settings", argc, argv);
  Fl::lock(); // pages with background work (sound, network) report via Fl::awake

  // other settings programs, not the apps of pages we have (fl-<page>.desktop,
  // or named here)
  std::set<std::string> ours{"fl-settings.desktop"};
  for (auto &p : fld::all_pages()) {
    ours.insert("fl-" + p.id + ".desktop");
    if (p.id == "updates") ours.insert("fl-packages.desktop");
    if (p.id == "sound") ours.insert("fl-mixer.desktop");
  }
  for (auto &d : fld::all_applications()) {
    bool settings = std::find(d.categories.begin(), d.categories.end(), "Settings") != d.categories.end();
    if (!settings || ours.count(d.id)) continue;
    g_external.push_back(d);
  }

  // fits small screens; the page area scrolls when it is shorter than a page
  int sx, sy, sw, sh;
  Fl::screen_work_area(sx, sy, sw, sh);
  const int SIDE = sw > 0 && sw < 960 ? 190 : 230;
  const int W = sw > 0 ? std::max(600, std::min(1020, sw - 40)) : 1020;
  // at most 85% of the screen height: room above and below on 768 px screens
  // (780 on a 1080p screen: the Appearance page fits, Wallpaper included)
  const int H = sh > 0 ? std::max(320, std::min(780, sh * 85 / 100)) : 680;
  const fld::Palette &p = fld::palette();
  g_win = new Fl_Double_Window(W, H, "Settings");
  Fl_Group *side = new Fl_Group(0, 0, SIDE, H);
  side->box(FL_FLAT_BOX);
  side->color(p.bg);
  g_search = new fld::SearchInput(10, 10, SIDE - 20, 30, "Search settings");
  g_search->tooltip("Search settings");
  g_search->when(FL_WHEN_CHANGED);
  g_search->callback([](Fl_Widget *, void *) {
    rebuild_list();
    // while searching, show the best match: a page whose title has a word
    // starting with the text, else the best single setting, else any page
    std::string q = fld::lower(fld::trim(g_search->value()));
    if (q.empty()) return;
    auto title_match = [&](const Entry &e) {
      std::string t = " " + fld::lower(e.label);
      return e.focus.empty() && t.find(" " + q) != std::string::npos;
    };
    const Entry *best = nullptr;
    for (auto &e : g_entries)
      if (!e.page_id.empty() && title_match(e)) { best = &e; break; }
    if (!best)
      for (auto &e : g_entries)
        if (!e.focus.empty()) { best = &e; break; }
    if (!best)
      for (auto &e : g_entries)
        if (!e.page_id.empty()) { best = &e; break; }
    if (!best) return;
    show_page(best->page_id);
    if (!best->focus.empty()) point_at(best->page_id, best->focus, false); // the typing goes on
  });
  g_list = new Fl_Hold_Browser(0, 50, SIDE, H - 50);
  g_list->box(FL_FLAT_BOX);
  g_list->color(p.bg);
  // 13 px text and 16 px icons: all the pages fit on a 768 px screen
  g_list->textsize(13);
  g_list->selection_color(p.accent);
  g_list->callback(list_cb);
  side->resizable(g_list);
  side->end();
  g_content = new ContentArea(SIDE, 0, W - SIDE, H);
  g_content->box(FL_FLAT_BOX);
  g_content->color(p.bg2);
  g_content->end();
  g_win->resizable(g_content);
  g_win->end();
  g_win->size_range(600, 320);
  fld::set_window_icon(g_win, "fl-settings");

  rebuild_list();
  if (start.empty() || [&] { for (auto &pg : fld::all_pages()) if (pg.id == start) return false; return true; }())
    start = fld::all_pages().empty() ? "" : fld::all_pages()[0].id;
  if (!start.empty()) show_page(start);
  fld::remember_window_size(g_win, "fl-settings");
  g_win->show(); // not show(argc, argv): it resets the theme colors
  return Fl::run();
}
