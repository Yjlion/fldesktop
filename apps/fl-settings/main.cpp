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
#include "settings_page.h"

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
    g_list->add(("@b@C8" + fld::escape_at(t)).c_str());
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
      g_list->icon(g_list->size(), fld::icon_first({p.icon, "preferences-system"}, 20));
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
    g_list->icon(g_list->size(), fld::icon_first({d.icon, "preferences-system"}, 20));
  }
  for (size_t i = 0; i < g_entries.size(); i++)
    if (g_entries[i].page_id == g_current) g_list->value((int)i + 1);
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
  for (size_t i = 0; i < g_entries.size(); i++)
    if (g_entries[i].page_id == id) g_list->value((int)i + 1);
  g_win->redraw();
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
  if (!e.page_id.empty()) show_page(e.page_id);
  else {
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
  const int H = sh > 0 ? std::max(320, std::min(680, sh - 60)) : 680;
  const fld::Palette &p = fld::palette();
  g_win = new Fl_Double_Window(W, H, "Settings");
  Fl_Group *side = new Fl_Group(0, 0, SIDE, H);
  side->box(FL_FLAT_BOX);
  side->color(p.bg);
  g_search = new Fl_Input(10, 10, SIDE - 20, 30);
  g_search->tooltip("Search settings");
  g_search->when(FL_WHEN_CHANGED);
  g_search->callback([](Fl_Widget *, void *) {
    rebuild_list();
    // jump to the first match while searching
    for (auto &e : g_entries)
      if (!e.page_id.empty()) { if (*g_search->value()) show_page(e.page_id); break; }
  });
  g_list = new Fl_Hold_Browser(0, 50, SIDE, H - 50);
  g_list->box(FL_FLAT_BOX);
  g_list->color(p.bg);
  g_list->textsize(14);
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
  g_win->show(); // not show(argc, argv): it resets the theme colors
  return Fl::run();
}
