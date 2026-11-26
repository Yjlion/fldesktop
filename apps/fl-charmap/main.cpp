// fl-charmap: character map for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Scrollbar.H>
#include <FL/Fl_Tile.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/fl_draw.H>

#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "charmap_data.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"

using charmap::utf8;

static std::vector<charmap::Block> g_blocks;
static charmap::NameDb g_names;
static std::vector<uint32_t> g_recent;
static std::string g_font_name = "Sans";
static Fl_Font g_font = FL_FREE_FONT;
static bool g_only_covered = false;

static std::string config_path() { return fld::fld_config_dir() + "/charmap.ini"; }

// ---- font coverage (fontconfig) ------------------------------------------------

class Coverage {
public:
  void font(const std::string &family) {
    if (cs_) FcCharSetDestroy(cs_);
    cs_ = nullptr;
    FcPattern *pat = FcNameParse((const FcChar8 *)family.c_str());
    if (!pat) return;
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult res;
    FcPattern *match = FcFontMatch(nullptr, pat, &res);
    FcPatternDestroy(pat);
    if (!match) return;
    FcCharSet *cs = nullptr;
    if (FcPatternGetCharSet(match, FC_CHARSET, 0, &cs) == FcResultMatch) cs_ = FcCharSetCopy(cs);
    FcPatternDestroy(match);
  }
  bool has(uint32_t cp) const { return !cs_ || FcCharSetHasChar(cs_, cp); }
  ~Coverage() { if (cs_) FcCharSetDestroy(cs_); }
private:
  FcCharSet *cs_ = nullptr;
};
static Coverage g_cov;

// ---- glyph grid ---------------------------------------------------------------------

class GlyphGrid;
static GlyphGrid *g_grid;
static void show_details(uint32_t cp);
static void insert_char(uint32_t cp);

class GlyphGrid : public Fl_Group {
public:
  std::vector<uint32_t> cps;
  int sel = -1;
  int cell = 52;
  int top = 0;
  Fl_Scrollbar *sb;

  GlyphGrid(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    sb = new Fl_Scrollbar(x + w - 14, y, 14, h);
    sb->type(FL_VERTICAL);
    sb->callback([](Fl_Widget *s, void *d) {
      GlyphGrid *g = (GlyphGrid *)d;
      g->top = ((Fl_Scrollbar *)s)->value();
      g->redraw();
    }, this);
    end();
  }
  int cols() const { return std::max(1, (w() - 14) / cell); }
  int rows() const { return ((int)cps.size() + cols() - 1) / cols(); }
  int visible_rows() const { return std::max(1, h() / cell); }

  void set(const std::vector<uint32_t> &v) {
    cps = v;
    sel = cps.empty() ? -1 : 0;
    top = 0;
    update_sb();
    redraw();
    if (sel >= 0) show_details(cps[sel]);
  }
  void update_sb() {
    int total = rows();
    if (top > std::max(0, total - visible_rows())) top = std::max(0, total - visible_rows());
    sb->value(top, visible_rows(), 0, std::max(total, visible_rows()));
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    sb->resize(X + W - 14, Y, 14, H);
    update_sb();
  }
  void ensure_visible() {
    if (sel < 0) return;
    int r = sel / cols();
    if (r < top) top = r;
    if (r >= top + visible_rows()) top = r - visible_rows() + 1;
    update_sb();
    redraw();
  }
  void draw() override {
    const fld::Palette &p = fld::palette();
    fl_push_clip(x(), y(), w() - 14, h());
    fl_color(p.bg2);
    fl_rectf(x(), y(), w(), h());
    int c = cols();
    int fs = cell * 11 / 20;
    for (int r = top; r < top + visible_rows() + 1; r++) {
      for (int k = 0; k < c; k++) {
        int i = r * c + k;
        if (i >= (int)cps.size()) break;
        int cx = x() + k * cell, cy = y() + (r - top) * cell;
        if (i == sel) {
          fl_color(p.accent);
          fl_rectf(cx, cy, cell, cell);
        }
        fl_color(p.border);
        fl_rect(cx, cy, cell + 1, cell + 1);
        uint32_t cp = cps[i];
        bool covered = g_cov.has(cp);
        std::string s = utf8(cp);
        fl_font(g_font, fs);
        fl_color(i == sel ? p.accent_fg : covered ? p.fg : p.fg_dim);
        int tw = (int)fl_width(s.c_str(), (int)s.size());
        fl_draw(s.c_str(), (int)s.size(), cx + (cell - tw) / 2, cy + cell / 2 + fs / 3);
      }
    }
    fl_pop_clip();
    draw_child(*sb);
  }
  int index_at(int ex, int ey) const {
    int k = (ex - x()) / cell, r = (ey - y()) / cell + top;
    if (k < 0 || k >= cols() || ex >= x() + w() - 14) return -1;
    int i = r * cols() + k;
    return i < (int)cps.size() ? i : -1;
  }
  int handle(int e) override {
    if (Fl::event_inside(sb) && (e == FL_PUSH || e == FL_DRAG || e == FL_RELEASE)) return Fl_Group::handle(e);
    switch (e) {
      case FL_FOCUS: case FL_UNFOCUS: redraw(); return 1;
      case FL_PUSH: {
        take_focus();
        int i = index_at(Fl::event_x(), Fl::event_y());
        if (i >= 0) {
          sel = i;
          redraw();
          show_details(cps[i]);
          if (Fl::event_clicks()) insert_char(cps[i]);
        }
        return 1;
      }
      case FL_MOUSEWHEEL:
        top = std::max(0, std::min(top + Fl::event_dy() * 2, rows() - visible_rows()));
        update_sb();
        redraw();
        return 1;
      case FL_KEYBOARD: {
        int k = Fl::event_key(), c = cols(), n = (int)cps.size();
        if (n == 0) return 0;
        int ns = sel;
        if (k == FL_Left) ns--;
        else if (k == FL_Right) ns++;
        else if (k == FL_Up) ns -= c;
        else if (k == FL_Down) ns += c;
        else if (k == FL_Page_Up) ns -= c * visible_rows();
        else if (k == FL_Page_Down) ns += c * visible_rows();
        else if (k == FL_Home) ns = 0;
        else if (k == FL_End) ns = n - 1;
        else if (k == FL_Enter || k == FL_KP_Enter || k == ' ') { if (sel >= 0) insert_char(cps[sel]); return 1; }
        else return 0;
        sel = std::max(0, std::min(n - 1, ns));
        ensure_visible();
        show_details(cps[sel]);
        return 1;
      }
    }
    return Fl_Group::handle(e);
  }
};

// ---- details / text --------------------------------------------------------------

static Fl_Box *g_big, *g_name, *g_info;
static Fl_Input *g_text;
static Fl_Hold_Browser *g_blocks_list;
static Fl_Input *g_search;
static uint32_t g_current = 0;

static const charmap::Block *block_of(uint32_t cp) {
  for (auto &b : g_blocks)
    if (cp >= b.first && cp <= b.last) return &b;
  return nullptr;
}

static void show_details(uint32_t cp) {
  g_current = cp;
  static std::string big, name, info;
  big = utf8(cp);
  g_big->copy_label(fld::escape_at(big).c_str()); // the escaped text is a temporary
  g_big->labelfont(g_font);
  name = g_names.name(cp);
  if (name.empty()) name = g_names.empty() ? "(install unicode-data for names)" : "(unassigned)";
  g_name->copy_label(fld::escape_at(name).c_str());
  char buf[256];
  std::string bytes;
  for (unsigned char c : big) {
    char h[8];
    snprintf(h, sizeof h, "%02X ", c);
    bytes += h;
  }
  const charmap::Block *b = block_of(cp);
  snprintf(buf, sizeof buf, "U+%04X\nDecimal: %u\nUTF-8: %s\nHTML: &#x%X;\nCategory: %s\nBlock: %s%s", cp, cp,
           bytes.c_str(), cp, g_names.category(cp).c_str(), b ? b->name.c_str() : "?",
           g_cov.has(cp) ? "" : "\n(not in the selected font)");
  info = buf;
  g_info->copy_label(fld::escape_at(info).c_str());
  g_big->redraw();
}

static void save_recent() {
  fld::Ini cfg;
  cfg.load(config_path());
  std::string s;
  for (uint32_t cp : g_recent) s += utf8(cp);
  cfg.set("Charmap", "Recent", s);
  cfg.set("Charmap", "Font", g_font_name);
  cfg.save(config_path());
}

static void insert_char(uint32_t cp) {
  std::string s = utf8(cp);
  g_text->insert(s.c_str());
  g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), cp), g_recent.end());
  g_recent.insert(g_recent.begin(), cp);
  if (g_recent.size() > 48) g_recent.resize(48);
  save_recent();
}

// ---- block list and filtering ------------------------------------------------------

static std::vector<uint32_t> block_chars(const charmap::Block &b) {
  std::vector<uint32_t> v;
  for (uint32_t cp = b.first; cp <= b.last; cp++) {
    if (!g_names.empty() && !g_names.printable(cp)) continue;
    if (g_names.empty() && (cp < 0x20 || (cp >= 0x7F && cp < 0xA0) || (cp >= 0xD800 && cp <= 0xDFFF))) continue;
    if (g_only_covered && !g_cov.has(cp)) continue;
    v.push_back(cp);
  }
  return v;
}

static void show_selected_block() {
  int line = g_blocks_list->value();
  if (line < 1) return;
  if (line == 1) { // recently used
    g_grid->set(g_recent);
    return;
  }
  const charmap::Block &b = g_blocks[line - 2];
  g_grid->set(block_chars(b));
}

static void do_search() {
  std::string q = fld::trim(g_search->value());
  if (q.empty()) { show_selected_block(); return; }
  std::vector<uint32_t> v = g_names.search(q, 2000);
  if (g_only_covered) v.erase(std::remove_if(v.begin(), v.end(), [](uint32_t c) { return !g_cov.has(c); }), v.end());
  g_grid->set(v);
}

static void select_block_for(uint32_t cp) {
  for (size_t i = 0; i < g_blocks.size(); i++)
    if (cp >= g_blocks[i].first && cp <= g_blocks[i].last) {
      g_blocks_list->value((int)i + 2);
      g_blocks_list->middleline((int)i + 2);
      show_selected_block();
      for (size_t k = 0; k < g_grid->cps.size(); k++)
        if (g_grid->cps[k] == cp) { g_grid->sel = (int)k; g_grid->ensure_visible(); }
      show_details(cp);
      return;
    }
}

static void set_font(const std::string &name) {
  g_font_name = name;
  static std::string font_storage;
  font_storage = name;
  Fl::set_font(FL_FREE_FONT, font_storage.c_str());
  g_cov.font(name);
  if (*g_search->value()) do_search();
  else show_selected_block();
  if (g_current) show_details(g_current);
  g_grid->redraw();
  save_recent();
}

int main(int argc, char **argv) {
  fld::init_app("fl-charmap", argc, argv);
  FcInit();
  std::string blocks_path = charmap::find_blocks(fld::fld_data_dir() + "/charmap");
  std::string text;
  if (!blocks_path.empty() && fld::read_file(blocks_path, text)) g_blocks = charmap::parse_blocks(text);
  if (g_blocks.empty()) g_blocks.push_back({0x20, 0x7E, "Basic Latin"});
  std::string ud = charmap::find_unicode_data();
  if (!ud.empty()) g_names.load(ud);

  fld::Ini cfg;
  cfg.load(config_path());
  g_font_name = cfg.get("Charmap", "Font", "Sans");
  std::string rec = cfg.get("Charmap", "Recent", "");
  for (size_t i = 0; i < rec.size();) {
    uint32_t cp = charmap::first_codepoint(rec.substr(i));
    g_recent.push_back(cp);
    i += utf8(cp).size();
  }

  const int W = 1000, H = 640, TB = 44, BOT = 48, SIDE = 230, DET = 250;
  const fld::Palette &p = fld::palette();
  Fl_Double_Window *win = new Fl_Double_Window(W, H, "Character Map");

  Fl_Group *top = new Fl_Group(0, 0, W, TB);
  top->box(FL_FLAT_BOX);
  top->color(p.bg);
  g_search = new Fl_Input(8, 8, 300, 28);
  g_search->tooltip("Search by name (e.g. \"arrow\"), U+2192 or a character");
  g_search->when(FL_WHEN_CHANGED);
  g_search->callback([](Fl_Widget *, void *) { do_search(); });
  Fl_Choice *fonts = new Fl_Choice(380, 8, 230, 28, "Font");
  Fl_Value_Slider *size = new Fl_Value_Slider(670, 8, 130, 28, "Size");
  size->type(FL_HOR_NICE_SLIDER);
  size->align(FL_ALIGN_LEFT);
  size->bounds(32, 96);
  size->step(4);
  size->value(52);
  size->callback([](Fl_Widget *s, void *) {
    g_grid->cell = (int)((Fl_Value_Slider *)s)->value();
    g_grid->update_sb();
    g_grid->redraw();
  });
  Fl_Check_Button *only = new fld::CheckButton(812, 8, 180, 28, "Only in this font");
  only->callback([](Fl_Widget *w, void *) {
    g_only_covered = ((Fl_Check_Button *)w)->value();
    if (*g_search->value()) do_search(); else show_selected_block();
  });
  top->resizable(nullptr);
  top->end();

  Fl_Tile *tile = new Fl_Tile(0, TB, W, H - TB - BOT);
  g_blocks_list = new Fl_Hold_Browser(0, TB, SIDE, H - TB - BOT);
  g_blocks_list->textsize(13);
  g_blocks_list->selection_color(p.accent);
  g_blocks_list->add("★ Recently Used");
  for (auto &b : g_blocks) g_blocks_list->add(fld::escape_at(b.name).c_str());
  g_blocks_list->callback([](Fl_Widget *, void *) {
    g_search->value("");
    show_selected_block();
  });
  g_grid = new GlyphGrid(SIDE, TB, W - SIDE - DET, H - TB - BOT);
  Fl_Group *det = new Fl_Group(W - DET, TB, DET, H - TB - BOT);
  det->box(FL_FLAT_BOX);
  det->color(p.bg);
  g_big = new Fl_Box(W - DET + 10, TB + 10, DET - 20, 130);
  g_big->box(FL_FLAT_BOX);
  g_big->color(p.bg2);
  g_big->labelsize(84);
  g_name = new Fl_Box(W - DET + 10, TB + 146, DET - 20, 50);
  g_name->labelfont(FL_HELVETICA_BOLD);
  g_name->labelsize(13);
  g_name->align(FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_TOP_LEFT);
  g_info = new Fl_Box(W - DET + 10, TB + 200, DET - 20, 150);
  g_info->labelsize(12);
  g_info->labelfont(FL_COURIER);
  g_info->align(FL_ALIGN_INSIDE | FL_ALIGN_TOP_LEFT | FL_ALIGN_WRAP);
  // right-click on a character (the click selects it first)
  fld::set_context_menu(g_grid, [](fld::Menu &m) {
    uint32_t cp = g_grid->sel >= 0 && g_grid->sel < (int)g_grid->cps.size() ? g_grid->cps[(size_t)g_grid->sel] : 0;
    int f = cp ? 0 : FL_MENU_INACTIVE;
    auto copy = [](const std::string &s) {
      Fl::copy(s.c_str(), (int)s.size(), 1);
      Fl::copy(s.c_str(), (int)s.size(), 0);
    };
    char hex[32];
    snprintf(hex, sizeof hex, "U+%04X", (unsigned)cp);
    std::string code = hex;
    snprintf(hex, sizeof hex, "&#x%X;", (unsigned)cp);
    std::string entity = hex;
    std::string name = g_names.name(cp);
    m.item("Add to Text", [cp] { insert_char(cp); }, f);
    m.item("Copy Character", [cp, copy] { copy(utf8(cp)); }, f).divider();
    m.item("Copy Code Point (" + code + ")", [code, copy] { copy(code); }, f);
    m.item("Copy HTML Entity (" + fld::Menu::text(entity) + ")", [entity, copy] { copy(entity); }, f);
    m.item("Copy Name", [name, copy] { copy(name); }, f | (name.empty() ? FL_MENU_INACTIVE : 0));
  });
  Fl_Button *add = new Fl_Button(W - DET + 10, TB + 356, DET - 20, 30, "Add to Text");
  add->callback([](Fl_Widget *, void *) { if (g_current) insert_char(g_current); });
  Fl_Button *copy1 = new Fl_Button(W - DET + 10, TB + 392, DET - 20, 30, "Copy Character");
  copy1->callback([](Fl_Widget *, void *) {
    std::string s = utf8(g_current);
    Fl::copy(s.c_str(), (int)s.size(), 1);
    Fl::copy(s.c_str(), (int)s.size(), 0);
  });
  Fl_Box *dfill = new Fl_Box(W - DET, TB + 430, DET, 10);
  det->resizable(dfill);
  det->end();
  tile->end();
  tile->size_range(g_blocks_list, 120, 120, 400, 10000);

  Fl_Group *bot = new Fl_Group(0, H - BOT, W, BOT);
  bot->box(FL_FLAT_BOX);
  bot->color(p.bg);
  g_text = new Fl_Input(80, H - BOT + 9, W - 290, 30, "Text:");
  g_text->textsize(18);
  Fl_Button *copy = new Fl_Button(W - 200, H - BOT + 9, 90, 30, "Copy");
  copy->callback([](Fl_Widget *, void *) {
    Fl::copy(g_text->value(), g_text->size(), 1);
    Fl::copy(g_text->value(), g_text->size(), 0);
  });
  Fl_Button *clear = new Fl_Button(W - 104, H - BOT + 9, 90, 30, "Clear");
  clear->callback([](Fl_Widget *, void *) { g_text->value(""); });
  bot->resizable(g_text);
  bot->end();
  win->resizable(tile);
  win->end();
  win->size_range(640, 400);
  fld::set_window_icon(win, "fl-charmap");

  // font list: FLTK lists the fonts known to fontconfig/pango
  int nfonts = Fl::set_fonts(nullptr);
  std::set<std::string> families;
  for (int i = 0; i < nfonts; i++) {
    int attr;
    const char *n = Fl::get_font_name((Fl_Font)i, &attr);
    if (n && !attr && *n) families.insert(n);
  }
  families.insert("Sans");
  families.insert("Serif");
  families.insert("Monospace");
  int fi = 0, idx = 0;
  for (auto &f : families) {
    fonts->add(fld::escape_at(f).c_str());
    if (f == g_font_name) fi = idx;
    idx++;
  }
  fonts->value(fi);
  fonts->callback([](Fl_Widget *w, void *) {
    const Fl_Menu_Item *m = ((Fl_Choice *)w)->mvalue();
    if (m) set_font(m->label());
  });
  // unescape the label we store ('@@' -> '@')
  set_font(g_font_name);

  g_blocks_list->value(g_recent.empty() ? 2 : 1);
  show_selected_block();
  // start with an argument: a character or U+XXXX
  if (argc > 1) {
    auto v = g_names.search(argv[1], 1);
    if (!v.empty()) select_block_for(v[0]);
  }
  win->show(); // not show(argc, argv): it resets the theme colors
  g_grid->take_focus();
  return Fl::run();
}
