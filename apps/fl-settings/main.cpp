// fl-settings: the fldesktop settings center
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstring>
#include <map>
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
static Fl_Group *g_content;
static std::map<std::string, Fl_Group *> g_created;
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

static void show_page(const std::string &id) {
  const fld::PageInfo *info = nullptr;
  for (auto &p : fld::all_pages()) if (p.id == id) info = &p;
  if (!info) return;
  for (auto &kv : g_created) kv.second->hide();
  Fl_Group *g;
  auto it = g_created.find(id);
  if (it == g_created.end()) {
    g_content->begin();
    g = info->create(g_content->x(), g_content->y(), g_content->w(), g_content->h());
    g_content->end();
    g_created[id] = g;
  } else {
    g = it->second;
  }
  g->resize(g_content->x(), g_content->y(), g_content->w(), g_content->h());
  g->show();
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

class ContentArea : public Fl_Group {
public:
  using Fl_Group::Fl_Group;
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    for (int i = 0; i < children(); i++) child(i)->resize(X, Y, W, H);
  }
};

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

  // other settings programs (not our own pages' apps)
  for (auto &d : fld::all_applications()) {
    bool settings = std::find(d.categories.begin(), d.categories.end(), "Settings") != d.categories.end();
    if (!settings || d.id == "fl-settings.desktop" || d.id == "fl-network.desktop" || d.id == "fl-mixer.desktop")
      continue;
    g_external.push_back(d);
  }

  const int W = 1020, H = 680, SIDE = 230;
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
  g_win->size_range(760, 480);
  fld::set_window_icon(g_win, "fl-settings");

  rebuild_list();
  if (start.empty() || [&] { for (auto &pg : fld::all_pages()) if (pg.id == start) return false; return true; }())
    start = fld::all_pages().empty() ? "" : fld::all_pages()[0].id;
  if (!start.empty()) show_page(start);
  g_win->show(); // not show(argc, argv): it resets the theme colors
  return Fl::run();
}
