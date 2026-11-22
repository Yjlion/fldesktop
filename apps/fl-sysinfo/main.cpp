// fl-sysinfo: system information (computer, processor, memory, graphics,
// storage, network, devices, batteries and sensors)
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Tile.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cstdio>
#include <unistd.h>
#include <string>
#include <vector>

#include "fld/icons.h"
#include "fld/menus.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "sysinfo.h"

namespace {

Fl_Double_Window *g_win;
Fl_Hold_Browser *g_pages;
class InfoView;
InfoView *g_info;
Fl_Box *g_status;
std::vector<std::string> g_ids;
int g_page = 0;

// The rows of a page: group headings, then key/value pairs. A row can be
// selected and copied.
class InfoView : public Fl_Hold_Browser {
public:
  sysinfo::Page page;
  struct Line {
    bool heading = false;
    std::string key, value; // value lines are separate Lines with an empty key
  };
  std::vector<Line> lines;

  InfoView(int x, int y, int w, int h) : Fl_Hold_Browser(x, y, w, h) {
    box(FL_FLAT_BOX);
    color(fld::palette().bg2);
    textsize(13);
    has_scrollbar(VERTICAL);
  }

  void set_page(const sysinfo::Page &p, bool keep_scroll) {
    int top = keep_scroll ? topline() : 1, sel = keep_scroll ? value() : 0;
    page = p;
    lines.clear();
    clear();
    for (auto &g : p.groups) {
      if (!lines.empty()) add_line({false, "", ""});
      add_line({true, g.title, ""});
      for (auto &r : g.rows) {
        auto parts = fld::split(r.value, '\n');
        for (size_t i = 0; i < parts.size(); i++) add_line({false, i ? "" : r.key, parts[i]});
      }
    }
    if (keep_scroll) {
      topline(std::min(top, size()));
      if (sel > 0 && sel <= size()) select(sel);
    }
  }

  // the key column fits the longest key
  int key_w() const {
    fl_font(FL_HELVETICA, textsize());
    int w = 120;
    for (auto &l : lines)
      if (!l.heading) w = std::max(w, (int)fl_width(l.key.c_str()) + 24);
    return std::min(w, this->w() / 2);
  }

  // the text of the selected row ("key: value"), or of the group of a heading
  std::string selected_text() {
    int i = value() - 1;
    if (i < 0 || i >= (int)lines.size()) return "";
    const Line &l = lines[(size_t)i];
    if (l.heading) {
      for (auto &g : page.groups)
        if (g.title == l.key) return sysinfo::report({sysinfo::Page{page.id, g.title, "", {g}}});
      return l.key;
    }
    // a value continued over several lines: its whole value
    int k = i;
    while (k > 0 && lines[(size_t)k].key.empty() && !lines[(size_t)k - 1].heading && !lines[(size_t)k - 1].value.empty())
      k--;
    std::string v;
    for (int j = k; j < (int)lines.size() && !lines[(size_t)j].heading && !lines[(size_t)j].value.empty() &&
                    (j == k || lines[(size_t)j].key.empty());
         j++)
      v += (j == k ? "" : "\n") + lines[(size_t)j].value;
    return lines[(size_t)k].key.empty() ? v : lines[(size_t)k].key + ": " + v;
  }

  int handle(int e) override {
    if (e == FL_KEYBOARD && (Fl::event_state() & FL_CTRL) && Fl::event_key() == 'c') {
      copy_text(selected_text());
      return 1;
    }
    return Fl_Hold_Browser::handle(e);
  }

  static void copy_text(const std::string &t) {
    if (t.empty()) return;
    Fl::copy(t.c_str(), (int)t.size(), 1);
    Fl::copy(t.c_str(), (int)t.size(), 0);
  }

protected:
  int item_height(void *it) const override {
    const Line *l = line_of(it);
    return l && l->heading ? 30 : 22;
  }
  void item_draw(void *it, int X, int Y, int W, int H) const override {
    const Line *l = line_of(it);
    if (!l) return;
    const fld::Palette &p = fld::palette();
    bool sel = item_selected(it);
    if (sel) {
      fl_color(Fl::focus() == this ? p.selection : fl_color_average(p.selection, p.bg2, 0.4f));
      fl_rectf(X, Y, W, H);
    }
    if (l->heading) {
      fl_font(FL_HELVETICA_BOLD, textsize() + 1);
      fl_color(sel ? p.accent_fg : p.fg);
      fl_draw(fld::escape_at(l->key).c_str(), X + 8, Y, W - 16, H, FL_ALIGN_LEFT | FL_ALIGN_BOTTOM | FL_ALIGN_CLIP);
      if (!sel) {
        fl_color(p.border);
        fl_xyline(X + 8, Y + H - 1, X + W - 8);
      }
      return;
    }
    int kw = const_cast<InfoView *>(this)->key_w();
    fl_font(FL_HELVETICA, textsize());
    fl_color(sel ? p.accent_fg : p.fg_dim);
    fl_draw(fld::escape_at(l->key).c_str(), X + 16, Y, kw - 24, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
    fl_color(sel ? p.accent_fg : p.fg);
    fl_draw(fld::escape_at(l->value).c_str(), X + 16 + kw, Y, W - kw - 24, H, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  }

private:
  void add_line(const Line &l) {
    lines.push_back(l);
    add("", (void *)(long)lines.size()); // data: index + 1
  }
  const Line *line_of(void *it) const {
    int n = lineno(it);
    return n > 0 && n <= (int)lines.size() ? &lines[(size_t)n - 1] : nullptr;
  }
};

void show_page(int i, bool keep_scroll = false) {
  if (i < 0 || i >= (int)g_ids.size()) return;
  bool same = i == g_page && keep_scroll;
  g_page = i;
  g_pages->value(i + 1);
  g_info->set_page(sysinfo::page(g_ids[(size_t)i]), same);
  g_win->copy_label((g_info->page.title + " – System Information").c_str());
  g_status->copy_label(i == (int)g_ids.size() - 1 ? "Updated every 2 seconds" : "");
}

// the batteries and sensors change: their page is read again while shown
void tick(void *) {
  if (g_ids[(size_t)g_page] == "power" && g_win->shown()) show_page(g_page, true);
  Fl::repeat_timeout(2.0, tick);
}

void save_report() {
  Fl_Native_File_Chooser fc;
  fc.title("Save Report");
  fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
  char host[256] = "";
  gethostname(host, sizeof host - 1);
  fc.preset_file((std::string("system-information-") + (host[0] ? host : "computer") + ".txt").c_str());
  fc.directory(fld::home_dir().c_str());
  fc.filter("Text files\t*.txt");
  if (fc.show() != 0 || !fc.filename()) return;
  std::string path = fc.filename();
  if (fld::extension(path).empty()) path += ".txt";
  if (!fld::write_file(path, sysinfo::report(sysinfo::all_pages())))
    fl_alert("Could not save %s", fld::escape_at(path).c_str());
  else g_status->copy_label(fld::escape_at("Saved " + fld::tilde(path)).c_str());
}

void copy_all() {
  InfoView::copy_text(sysinfo::report(sysinfo::all_pages()));
  g_status->copy_label("Copied the whole report");
}

void info_menu(fld::Menu &m) {
  std::string t = g_info->selected_text();
  m.item("&Copy", [t] { InfoView::copy_text(t); }, t.empty() ? FL_MENU_INACTIVE : 0, FL_CTRL + 'c');
  m.item("Copy &Page", [] { InfoView::copy_text(sysinfo::report({g_info->page})); });
  m.item("Copy &All", [] { copy_all(); }, FL_MENU_DIVIDER);
  m.item("&Save Report…", [] { save_report(); }, 0, FL_CTRL + 's');
  m.item("&Refresh", [] { show_page(g_page, true); }, 0, FL_F + 5);
}

int keys(int e) {
  if (e != FL_SHORTCUT) return 0;
  int k = Fl::event_key();
  bool ctrl = Fl::event_state() & FL_CTRL;
  if (k == FL_F + 5) { show_page(g_page, true); return 1; }
  if (ctrl && k == 's') { save_report(); return 1; }
  if (ctrl && (k == 'q' || k == 'w')) { g_win->hide(); return 1; }
  if (ctrl && k >= '1' && k <= '9') { show_page(k - '1'); return 1; }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  g_ids = sysinfo::page_ids();
  std::string start = "overview";
  bool print = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-sysinfo [--page ID] [--report]\n  pages: %s\n  --report prints the report and quits\n",
             fld::join(g_ids, ", ").c_str());
      return 0;
    }
    if ((a == "--page" || a == "-p") && i + 1 < argc) start = argv[++i];
    else if (fld::starts_with(a, "--page=")) start = a.substr(7);
    else if (a == "--report") print = true;
  }
  if (print) {
    fputs(sysinfo::report(sysinfo::all_pages()).c_str(), stdout);
    return 0;
  }
  fld::init_app("fl-sysinfo", argc, argv);

  const fld::Palette &p = fld::palette();
  const int W = 860, H = 600, TB = 42, ST = 24, SW = 190;
  g_win = new Fl_Double_Window(W, H, "System Information");
  fld::Toolbar *bar = new fld::Toolbar(0, 0, W, TB);
  bar->color(p.bg);
  bar->add_button("view-refresh", "Read again (F5)", [](Fl_Widget *, void *) { show_page(g_page, true); }, nullptr, "Refresh");
  bar->add_button("edit-copy", "Copy the whole report", [](Fl_Widget *, void *) { copy_all(); }, nullptr, "Copy All");
  bar->add_button("document-save", "Save the whole report as a text file (Ctrl+S)",
                  [](Fl_Widget *, void *) { save_report(); }, nullptr, "Save Report…");
  Fl_Box *fill = new Fl_Box(bar->next_x(), 0, 10, TB);
  bar->add(fill);
  bar->resizable(fill);
  bar->end();

  Fl_Tile *tile = new Fl_Tile(0, TB, W, H - TB - ST);
  g_pages = new Fl_Hold_Browser(0, TB, SW, H - TB - ST);
  g_pages->box(FL_FLAT_BOX);
  g_pages->color(p.bg);
  g_pages->textsize(14);
  g_pages->selection_color(p.accent);
  g_pages->clear_visible_focus();
  static const char *fallback[] = {"computer", "cpu", "media-flash", "video-display", "drive-harddisk", "network-wired",
                                   "input-keyboard", "battery"};
  static const char *titles[] = {"Overview", "Processor", "Memory", "Graphics", "Storage", "Network", "Devices",
                                 "Power & Sensors"};
  for (size_t i = 0; i < g_ids.size(); i++) {
    g_pages->add((std::string("  ") + fld::escape_at(i < 8 ? titles[i] : g_ids[i])).c_str());
    std::vector<std::string> icons;
    if (g_ids[i] == "processor") icons = {"cpu", "computer"};
    else if (g_ids[i] == "memory") icons = {"media-flash", "drive-removable-media"};
    else if (g_ids[i] == "devices") icons = {"preferences-desktop-peripherals", "input-keyboard", "computer"};
    else icons = {i < 8 ? fallback[i] : "computer", "computer"};
    g_pages->icon(g_pages->size(), fld::icon_first(icons, 22));
  }
  g_pages->callback([](Fl_Widget *w, void *) {
    int line = ((Fl_Hold_Browser *)w)->value();
    if (line > 0) show_page(line - 1);
    else ((Fl_Hold_Browser *)w)->value(g_page + 1);
  });
  g_info = new InfoView(SW, TB, W - SW, H - TB - ST);
  fld::set_context_menu(g_info, info_menu);
  tile->end();
  tile->resizable(g_info);
  tile->size_range(g_pages, 140, 100, 360, 10000);

  g_status = new Fl_Box(0, H - ST, W, ST);
  g_status->box(FL_FLAT_BOX);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  g_status->labelsize(12);
  g_status->labelcolor(p.fg_dim);
  g_win->resizable(tile);
  g_win->end();
  g_win->size_range(520, 340);
  fld::set_window_icon(g_win, "fl-sysinfo");
  Fl::add_handler(keys);

  int first = 0;
  for (size_t i = 0; i < g_ids.size(); i++)
    if (g_ids[i] == start) first = (int)i;
  show_page(first);
  Fl::add_timeout(2.0, tick);
  g_win->show();
  g_info->take_focus();
  return Fl::run();
}
