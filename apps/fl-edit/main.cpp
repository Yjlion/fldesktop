#include "fld/json.h" // first: X11 headers define Bool
// fl-edit: text editor for fldesktop
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Menu_Bar.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstring>
#include <cctype>
#include <functional>
#include <map>
#include <strings.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/tab_bar.h"
#include "fld/tab_move.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "lsp.h"
#include "syntax.h"

static Fl_Double_Window *g_win;
static Fl_Menu_Bar *g_menu;
static fld::TabBar *g_tabbar;
static Fl_Group *g_area; // holds the documents; only the current one is shown
static Fl_Box *g_status;

struct Settings {
  bool line_numbers = true, wrap = false, highlight = true, spaces = false, auto_indent = true;
  bool brackets = true, minimap = true, lsp = true, restore_session = true;
  int font_size = 14, tab_width = 4;
  std::string theme = "auto";
  std::vector<std::string> recent;
} S;

static const int MAX_HIGHLIGHT = 4 << 20; // skip highlighting huge files

// ---- style table ---------------------------------------------------------------

// each style three times: plain, with an error underline and with a warning underline
static const int NSTYLES = syntax::STYLE_COUNT * 3;
static Fl_Text_Display::Style_Table_Entry g_styles[NSTYLES];

static Fl_Color rgb(unsigned v) { return fl_rgb_color((v >> 16) & 255, (v >> 8) & 255, v & 255); }
static const syntax::Theme &cur_theme() { return syntax::theme(S.theme, fld::palette().dark); }

static void make_styles() {
  const syntax::Theme &th = cur_theme();
  Fl_Font f = fld::mono_font();
  int s = S.font_size;
  for (int i = 0; i < syntax::STYLE_COUNT; i++) {
    Fl_Font fi = f + (th.bold[i] ? FL_BOLD : 0) + (th.italic[i] ? FL_ITALIC : 0);
    g_styles[i] = {rgb(th.colors[i]), fi, s, 0, 0};
  }
  // the matching bracket gets a background
  g_styles[syntax::BRACKET - 'A'].attr = Fl_Text_Display::ATTR_BGCOLOR;
  g_styles[syntax::BRACKET - 'A'].bgcolor = rgb(th.bracket_bg);
  for (int i = 0; i < syntax::STYLE_COUNT; i++) {
    g_styles[i + syntax::STYLE_COUNT] = g_styles[i];
    g_styles[i + syntax::STYLE_COUNT].attr |= Fl_Text_Display::ATTR_SPELLING;
    g_styles[i + 2 * syntax::STYLE_COUNT] = g_styles[i];
    g_styles[i + 2 * syntax::STYLE_COUNT].attr |= Fl_Text_Display::ATTR_GRAMMAR;
  }
}

// ---- documents --------------------------------------------------------------------

class Doc;
static std::vector<Doc *> g_docs;
static void update_status();
static void update_tab_label(Doc *d);

// more carets than the editor's own one (Ctrl+D, Alt+click, Ctrl+Alt+Up/Down):
// typing, deleting and moving happen at all of them
struct Caret {
  int pos, anchor;
};

class Editor : public Fl_Text_Editor {
public:
  Doc *doc;
  std::vector<Caret> extra;
  int last_top = -1;
  Editor(int x, int y, int w, int h, Doc *d) : Fl_Text_Editor(x, y, w, h), doc(d) {}
  int handle(int e) override;
  void draw() override;
  int handle_carets(int k, int st);
  void multi_edit(const std::string &text, int dir);
  void move_carets(int k);
  void add_next_occurrence();
  void add_caret_vertical(int dir);
  bool xy_of(int pos, int &X, int &Y) const { return position_to_xy(pos, &X, &Y) != 0; }
  int line_height() const { return mMaxsize; }
  int visible_lines() const { return mNVisibleLines; }
  int text_x() const { return text_area.x; }
};

// a picture of the whole document beside the text; click or drag to scroll
class Minimap : public Fl_Widget {
public:
  Doc *doc;
  int first = 0; // the first line drawn
  Minimap(int x, int y, int w, int h, Doc *d) : Fl_Widget(x, y, w, h), doc(d) {}
  void draw() override;
  int handle(int e) override;
};
static const int MINIMAP_W = 96;

class Doc : public Fl_Group {
public:
  std::string path;
  bool modified = false;
  bool loading = false;
  Fl_Text_Buffer *buf, *style;
  Editor *ed;
  syntax::Language lang = syntax::NONE;
  bool lang_forced = false;        // chosen in View > Language
  std::string tab_label;
  std::string lexed, lexed_styles; // text and styles of the last rehighlight
  bool dirty = true;               // edited since then
  std::vector<int> marks;          // positions styled as BRACKET
  std::string shown_styles;        // lexed_styles with the diagnostics' underlines
  Minimap *mini = nullptr;
  lspc::Client *lsp = nullptr;     // the language server, when there is one
  std::vector<lsp::Diagnostic> diags;

  Doc(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    buf = new Fl_Text_Buffer();
    style = new Fl_Text_Buffer();
    buf->tab_distance(S.tab_width);
    ed = new Editor(x, y, w, h, this);
    ed->buffer(buf);
    ed->textfont(fld::mono_font());
    ed->textsize(S.font_size);
    ed->box(FL_FLAT_BOX);
    ed->cursor_style(Fl_Text_Display::SIMPLE_CURSOR);
    ed->linenumber_font(fld::mono_font());
    ed->linenumber_size(S.font_size - 2);
    ed->scrollbar_size(12);
    mini = new Minimap(x + w - MINIMAP_W, y, MINIMAP_W, h, this);
    resizable(ed);
    end();
    fld::no_edit_menu(ed); // the editor has its own menu
    buf->add_modify_callback(modify_cb, this);
    apply_settings();
  }
  ~Doc() override {
    Fl::remove_timeout(lsp_change_later, this);
    if (lsp) lspc::closed(lsp, path_open);
    buf->remove_modify_callback(modify_cb, this);
    ed->buffer(nullptr);
    ed->highlight_data(nullptr, nullptr, 0, 0, nullptr, nullptr);
    delete buf;
    delete style;
  }

  // the style buffer is used for highlighting and for bracket matching
  bool styled() const { return (S.highlight || S.brackets) && buf->length() < MAX_HIGHLIGHT; }
  syntax::Language effective_lang() const { return S.highlight ? lang : syntax::NONE; }

  void layout() {
    bool m = S.minimap && buf->length() < MAX_HIGHLIGHT;
    ed->resize(x(), y(), w() - (m ? MINIMAP_W : 0), h());
    mini->resize(x() + w() - MINIMAP_W, y(), MINIMAP_W, h());
    if (m) mini->show(); else mini->hide();
    redraw();
  }
  void resize(int X, int Y, int W, int H) override {
    Fl_Widget::resize(X, Y, W, H);
    layout();
  }

  void apply_settings() {
    make_styles();
    layout();
    const syntax::Theme &th = cur_theme();
    ed->color(rgb(th.bg));
    ed->textcolor(rgb(th.colors[0]));
    ed->cursor_color(rgb(th.cursor));
    ed->selection_color(rgb(th.selection));
    ed->linenumber_bgcolor(rgb(th.line_bg));
    ed->linenumber_fgcolor(rgb(th.line_fg));
    ed->textsize(S.font_size);
    ed->linenumber_size(std::max(8, S.font_size - 2));
    ed->linenumber_width(S.line_numbers ? std::max(40, (int)(S.font_size * 3.5)) : 0);
    ed->wrap_mode(S.wrap ? Fl_Text_Display::WRAP_AT_BOUNDS : Fl_Text_Display::WRAP_NONE, 0);
    buf->tab_distance(S.tab_width);
    if (styled())
      ed->highlight_data(style, g_styles, NSTYLES, 'A', nullptr, nullptr);
    else
      ed->highlight_data(nullptr, nullptr, 0, 0, nullptr, nullptr);
    rehighlight();
    ed->redraw();
  }

  void rehighlight() {
    marks.clear();
    if (!styled()) return;
    char *text = buf->text();
    lexed = text;
    free(text);
    syntax::highlight(effective_lang(), lexed, lexed_styles);
    dirty = false;
    shown_styles = lexed_styles;
    overlay_diagnostics();
    style->text(shown_styles.c_str());
    ed->redisplay_range(0, buf->length());
    update_brackets();
  }

  // Marks the bracket at (or just before) the cursor and its partner.
  void update_brackets() {
    if (!styled()) return;
    for (int p : marks)
      if (p < style->length() && p < (int)shown_styles.size()) {
        char o[2] = {shown_styles[p], 0};
        style->replace(p, p + 1, o);
        ed->redisplay_range(p, p + 1);
      }
    marks.clear();
    if (!S.brackets || dirty || (int)lexed.size() != buf->length()) return;
    int pos = ed->insert_position();
    long m = -1;
    int at = pos;
    if (pos < buf->length()) m = syntax::match_bracket(lexed, lexed_styles, (size_t)pos);
    if (m < 0 && pos > 0) {
      at = pos - 1;
      m = syntax::match_bracket(lexed, lexed_styles, (size_t)at);
    }
    if (m < 0) return;
    for (int p : {at, (int)m}) {
      style->replace(p, p + 1, "L");
      ed->redisplay_range(p, p + 1);
      marks.push_back(p);
    }
  }

  static void highlight_later(void *d) { static_cast<Doc *>(d)->rehighlight(); }

  // ---- language server

  // a buffer position for an LSP (line, UTF-16 column)
  int pos_of(int line, int col16) const {
    int ls = buf->skip_lines(0, line);
    char *t = buf->line_text(ls);
    int b = lsp::byte_col(t, col16);
    free(t);
    return std::min(ls + b, buf->length());
  }
  void lsp_position(int pos, int &line, int &col16) const {
    line = buf->count_lines(0, pos);
    int ls = buf->line_start(pos);
    char *t = buf->text_range(ls, pos);
    col16 = lsp::utf16_col(t, pos - ls);
    free(t);
  }
  std::string lsp_lang() const { return lsp::language_id(syntax::language_name(lang)); }
  void lsp_open() {
    if (lsp || !S.lsp || path.empty() || buf->length() >= MAX_HIGHLIGHT) return;
    char *t = buf->text();
    lsp = lspc::attach(path, lsp_lang(), t);
    free(t);
  }
  void lsp_reopen() { // after a new path or language
    if (lsp) lspc::closed(lsp, path_open);
    lsp = nullptr;
    diags.clear();
    lsp_open();
    path_open = path;
  }
  std::string path_open; // the path the server knows the document by
  static void lsp_change_later(void *v) {
    Doc *d = static_cast<Doc *>(v);
    if (!d->lsp) return;
    char *t = d->buf->text();
    lspc::change(d->lsp, d->path, t);
    free(t);
  }
  void lsp_flush() {
    if (Fl::has_timeout(lsp_change_later, this)) {
      Fl::remove_timeout(lsp_change_later, this);
      lsp_change_later(this);
    }
  }
  // underlines the diagnostics in shown_styles
  void overlay_diagnostics() {
    for (auto &dg : diags) {
      if (dg.severity > 2) continue;
      int a = pos_of(dg.line, dg.col), b = pos_of(dg.end_line, dg.end_col);
      if (b <= a) b = std::min(buf->word_end(a) > a ? buf->word_end(a) : a + 1, buf->length());
      int add = dg.severity == 1 ? syntax::STYLE_COUNT : 2 * syntax::STYLE_COUNT;
      for (int p = a; p < b && p < (int)shown_styles.size(); p++) {
        if (buf->byte_at(p) == '\n') continue;
        int c = (shown_styles[(size_t)p] - 'A') % syntax::STYLE_COUNT;
        shown_styles[(size_t)p] = (char)('A' + c + add);
      }
    }
  }
  void set_diagnostics(const std::vector<lsp::Diagnostic> &d) {
    diags = d;
    std::sort(diags.begin(), diags.end(), [](const lsp::Diagnostic &x, const lsp::Diagnostic &y) {
      return x.line != y.line ? x.line < y.line : x.col < y.col;
    });
    if (!styled() || dirty) return; // the next rehighlight shows them
    shown_styles = lexed_styles;
    overlay_diagnostics();
    style->text(shown_styles.c_str());
    marks.clear();
    update_brackets();
    ed->redisplay_range(0, buf->length());
    mini->redraw();
  }

  static void modify_cb(int pos, int ins, int del, int, const char *deleted, void *data) {
    Doc *d = static_cast<Doc *>(data);
    if (d->loading || (ins == 0 && del == 0)) return;
    // tab_distance() "replaces" the whole text with itself to force a redisplay
    // (without the deleted text in some FLTK versions, with it in others)
    if (del > 0 && ins == del) {
      if (!deleted) return;
      char *now = d->buf->text_range(pos, pos + ins);
      bool same = memcmp(now, deleted, (size_t)del) == 0;
      free(now);
      if (same) return;
    }
    d->dirty = true;
    d->marks.clear();
    // keep the style buffer the same length right away, re-lex shortly after
    if (d->style->length() != d->buf->length() && d->styled()) {
      if (del) d->style->remove(pos, std::min(pos + del, d->style->length()));
      if (ins) {
        std::string fill(ins, 'A');
        d->style->insert(pos, fill.c_str());
      }
    }
    Fl::remove_timeout(highlight_later, d);
    Fl::add_timeout(0.15, highlight_later, d);
    if (d->lsp) {
      Fl::remove_timeout(lsp_change_later, d);
      Fl::add_timeout(0.4, lsp_change_later, d);
    }
    d->mini->redraw();
    if (!d->modified) {
      d->modified = true;
      update_tab_label(d);
    }
    update_status();
  }

  std::string display_name() const { return path.empty() ? "Untitled" : fld::basename(path); }

  bool load(const std::string &p) {
    std::string data;
    if (!fld::read_file(p, data, 256u << 20)) {
      fl_alert("Cannot open %s:\n%s", p.c_str(), strerror(errno));
      return false;
    }
    loading = true;
    buf->text(data.c_str());
    loading = false;
    buf->canUndo(0); // reset the undo history
    buf->canUndo(1);
    path = fld::abs_path(p);
    modified = false;
    std::string first = data.substr(0, data.find('\n'));
    if (!lang_forced) lang = syntax::detect(path, first);
    apply_settings();
    update_tab_label(this);
    lsp_reopen();
    return true;
  }

  bool save(const std::string &p) {
    char *text = buf->text();
    bool ok = fld::write_file(p, text);
    free(text);
    if (!ok) {
      fl_alert("Cannot save %s:\n%s", p.c_str(), strerror(errno));
      return false;
    }
    if (path != fld::abs_path(p)) {
      path = fld::abs_path(p);
      char *t = buf->text();
      std::string s(t);
      free(t);
      if (!lang_forced) lang = syntax::detect(path, s.substr(0, s.find('\n')));
      apply_settings();
      lsp_reopen();
    } else if (lsp) {
      lsp_flush();
      lspc::saved(lsp, path);
    }
    modified = false;
    update_tab_label(this);
    return true;
  }
};

static Doc *g_current = nullptr;
static Doc *current() { return g_current; }

// ---- minimap -----------------------------------------------------------------------------

static const int MINI_LH = 2; // pixels per line

void Minimap::draw() {
  const syntax::Theme &th = cur_theme();
  Fl_Text_Buffer *buf = doc->buf, *style = doc->style;
  Editor *ed = doc->ed;
  Fl_Color bg = rgb(th.bg);
  fl_push_clip(x(), y(), w(), h());
  fl_rectf(x(), y(), w(), h(), bg);
  fl_color(rgb(th.line_bg));
  fl_yxline(x(), y(), y() + h() - 1);
  int lines = buf->count_lines(0, buf->length()) + 1;
  int rows = h() / MINI_LH;
  int vis = std::max(1, ed->visible_lines());
  int top = std::max(0, ed->scroll_row() - 1);
  if (lines <= rows) first = 0;
  else first = (int)((long long)top * (lines - rows) / std::max(1, lines - vis));
  first = std::max(0, std::min(first, std::max(0, lines - rows)));
  // the part that shows in the editor
  fl_rectf(x() + 1, y() + (top - first) * MINI_LH, w() - 1, vis * MINI_LH, rgb(th.line_bg));
  bool styled = doc->styled() && style->length() == buf->length();
  int pos = buf->skip_lines(0, first), len = buf->length();
  for (int r = 0; r < rows && pos < len; r++) {
    int col = 0, X0 = x() + 4;
    int end = buf->line_end(pos);
    int run_start = -1, run_style = -1;
    auto flush = [&](int upto) {
      if (run_start < 0) return;
      fl_color(rgb(th.colors[run_style % syntax::STYLE_COUNT]));
      fl_rectf(X0 + run_start, y() + r * MINI_LH, upto - run_start, MINI_LH - 1 > 0 ? MINI_LH - 1 : 1);
      run_start = -1;
    };
    for (int p = pos; p < end && col < w() - 6; p++) {
      unsigned char c = (unsigned char)buf->byte_at(p);
      if ((c & 0xC0) == 0x80) continue; // UTF-8 continuation
      if (c == '\t') {
        flush(col);
        col += S.tab_width - col % S.tab_width;
        continue;
      }
      if (c == ' ') {
        flush(col);
        col++;
        continue;
      }
      int st = styled ? style->byte_at(p) - 'A' : 0;
      if (st < 0 || st >= NSTYLES) st = 0;
      if (run_start >= 0 && st != run_style) flush(col);
      if (run_start < 0) {
        run_start = col;
        run_style = st;
      }
      col++;
    }
    flush(std::min(col, w() - 6));
    pos = end + 1;
  }
  // problems: marks at the right edge
  for (auto &dg : doc->diags) {
    if (dg.severity > 2) continue;
    int yy = lines <= rows ? dg.line * MINI_LH : (int)((long long)dg.line * h() / lines);
    fl_rectf(x() + w() - 4, y() + yy, 4, 3, dg.severity == 1 ? FL_RED : fl_rgb_color(230, 160, 0));
  }
  fl_pop_clip();
}

int Minimap::handle(int e) {
  switch (e) {
    case FL_PUSH:
    case FL_DRAG: {
      int lines = doc->buf->count_lines(0, doc->buf->length()) + 1;
      int vis = std::max(1, doc->ed->visible_lines());
      int line = first + (Fl::event_y() - y()) / MINI_LH - vis / 2;
      line = std::max(0, std::min(line, lines - vis));
      doc->ed->scroll(line + 1, 0);
      redraw();
      return 1;
    }
    case FL_MOUSEWHEEL:
      return doc->ed->handle(e);
    case FL_RELEASE: return 1;
  }
  return Fl_Widget::handle(e);
}

// ---- carets ----------------------------------------------------------------------------

void Editor::draw() {
  Fl_Text_Editor::draw();
  if (scroll_row() != last_top) {
    last_top = scroll_row();
    if (doc->mini) doc->mini->redraw();
  }
  if (extra.empty()) return;
  fl_push_clip(text_area.x, text_area.y, text_area.w, text_area.h);
  Fl_Color cc = cursor_color();
  for (auto &c : extra) {
    int X, Y;
    if (c.anchor != c.pos) {
      int a = std::min(c.pos, c.anchor), b = std::max(c.pos, c.anchor), X2, Y2;
      if (xy_of(a, X, Y) && xy_of(b, X2, Y2) && Y2 == Y) {
        fl_color(selection_color());
        fl_rectf(X, Y + mMaxsize - 3, X2 - X, 3);
      }
    }
    if (xy_of(c.pos, X, Y)) {
      fl_color(cc);
      fl_rectf(X - 1, Y, 2, mMaxsize);
    }
  }
  fl_pop_clip();
}

// the editor's own caret and selection as a Caret
static Caret primary_caret(Editor *ed) {
  int a, b, pos = ed->insert_position();
  if (ed->buffer()->selection_position(&a, &b) && b > a) return {pos, pos == a ? b : a};
  return {pos, pos};
}

void Editor::multi_edit(const std::string &text, int dir) {
  Fl_Text_Buffer *b = buffer();
  std::vector<std::pair<Caret, bool>> all; // the caret, is the primary one
  all.push_back({primary_caret(this), true});
  for (auto &c : extra) all.push_back({c, false});
  std::sort(all.begin(), all.end(), [](const std::pair<Caret, bool> &x, const std::pair<Caret, bool> &y) {
    return std::min(x.first.pos, x.first.anchor) < std::min(y.first.pos, y.first.anchor);
  });
  b->unselect();
  int delta = 0, last_end = -1, primary = 0;
  std::vector<Caret> out;
  for (auto &ce : all) {
    Caret &c = ce.first;
    int s = std::min(c.pos, c.anchor) + delta, e = std::max(c.pos, c.anchor) + delta;
    if (s < last_end) { // overlaps the one before: drop it
      if (ce.second) primary = last_end;
      continue;
    }
    int at = s;
    if (e > s) {
      b->remove(s, e);
      delta -= e - s;
    } else if (dir < 0 && s > 0) {
      int p = b->prev_char(s);
      b->remove(p, s);
      delta -= s - p;
      at = p;
    } else if (dir > 0 && s < b->length()) {
      int n = b->next_char(s);
      b->remove(s, n);
      delta -= n - s;
    }
    if (!text.empty()) {
      b->insert(at, text.c_str());
      delta += (int)text.size();
      at += (int)text.size();
    }
    last_end = at;
    if (ce.second) primary = at;
    else out.push_back({at, at});
  }
  extra = out;
  insert_position(primary);
  show_insert_position();
  redraw();
}

void Editor::move_carets(int k) {
  Fl_Text_Buffer *b = buffer();
  auto move = [&](int pos) {
    switch (k) {
      case FL_Left: return pos > 0 ? b->prev_char(pos) : 0;
      case FL_Right: return pos < b->length() ? b->next_char(pos) : pos;
      case FL_Home: return b->line_start(pos);
      case FL_End: return b->line_end(pos);
      case FL_Up:
      case FL_Down: {
        int ls = b->line_start(pos), col = pos - ls;
        int nl = k == FL_Up ? (ls > 0 ? b->line_start(ls - 1) : -1) : (b->line_end(pos) < b->length() ? b->line_end(pos) + 1 : -1);
        if (nl < 0) return pos;
        return std::min(nl + col, b->line_end(nl));
      }
    }
    return pos;
  };
  for (auto &c : extra) c.pos = c.anchor = move(c.pos);
  int p = move(insert_position());
  b->unselect();
  insert_position(p);
  // carets that meet become one
  std::sort(extra.begin(), extra.end(), [](const Caret &x, const Caret &y) { return x.pos < y.pos; });
  extra.erase(std::unique(extra.begin(), extra.end(), [](const Caret &x, const Caret &y) { return x.pos == y.pos; }),
              extra.end());
  extra.erase(std::remove_if(extra.begin(), extra.end(), [p](const Caret &c) { return c.pos == p; }), extra.end());
  show_insert_position();
  redraw();
}

// keys while there are more carets; returns 1 when handled
int Editor::handle_carets(int k, int st) {
  bool mods = st & (FL_CTRL | FL_ALT | FL_META);
  if (k == FL_Escape) {
    extra.clear();
    redraw();
    return 1;
  }
  if (k == FL_BackSpace && !mods) { multi_edit("", -1); return 1; }
  if (k == FL_Delete && !mods) { multi_edit("", 1); return 1; }
  if ((k == FL_Enter || k == FL_KP_Enter) && !mods) { multi_edit("\n", 0); return 1; }
  if (k == FL_Tab && !mods) { multi_edit(S.spaces ? std::string(S.tab_width, ' ') : "\t", 0); return 1; }
  if ((k == FL_Left || k == FL_Right || k == FL_Home || k == FL_End || k == FL_Up || k == FL_Down) &&
      !(st & (FL_SHIFT | FL_CTRL | FL_ALT))) {
    move_carets(k);
    return 1;
  }
  if (!mods && Fl::event_length() > 0 && (unsigned char)Fl::event_text()[0] >= 32 && Fl::event_text()[0] != 127) {
    multi_edit(std::string(Fl::event_text(), (size_t)Fl::event_length()), 0);
    return 1;
  }
  // Ctrl+D and the others are menu shortcuts; anything else ends the extra carets
  if (!((st & FL_CTRL) && (k == 'd' || k == FL_Up || k == FL_Down)) && !(k >= FL_Shift_L && k <= FL_Meta_R)) {
    extra.clear();
    redraw();
  }
  return 0;
}

// Ctrl+D: selects the word at the caret; with a selection, also the next place with the same text
void Editor::add_next_occurrence() {
  Fl_Text_Buffer *b = buffer();
  int a, e;
  if (!b->selection_position(&a, &e) || e <= a) {
    int pos = insert_position();
    a = b->word_start(pos);
    e = b->word_end(pos);
    if (e > a) {
      b->select(a, e);
      insert_position(e);
    }
    redraw();
    return;
  }
  char *t = b->text_range(a, e);
  std::string needle(t);
  free(t);
  int from = e;
  for (auto &c : extra) from = std::max(from, std::max(c.pos, c.anchor));
  int found;
  bool ok = b->search_forward(from, needle.c_str(), &found, 1) != 0;
  if (!ok) ok = b->search_forward(0, needle.c_str(), &found, 1) != 0;
  if (!ok || found == a) return;
  for (auto &c : extra)
    if (std::min(c.pos, c.anchor) == found) return; // all are taken
  extra.push_back(primary_caret(this));
  b->select(found, found + (int)needle.size());
  insert_position(found + (int)needle.size());
  show_insert_position();
  redraw();
}

void Editor::add_caret_vertical(int dir) {
  Fl_Text_Buffer *b = buffer();
  int pos = insert_position(), ls = b->line_start(pos), col = pos - ls;
  int nl = dir < 0 ? (ls > 0 ? b->line_start(ls - 1) : -1) : (b->line_end(pos) < b->length() ? b->line_end(pos) + 1 : -1);
  if (nl < 0) return;
  extra.push_back({pos, pos});
  b->unselect();
  insert_position(std::min(nl + col, b->line_end(nl)));
  show_insert_position();
  redraw();
}

static int doc_index(Doc *d) {
  auto it = std::find(g_docs.begin(), g_docs.end(), d);
  return it == g_docs.end() ? -1 : (int)(it - g_docs.begin());
}

static void refresh_tabbar() {
  std::vector<fld::TabInfo> tabs;
  for (auto *d : g_docs) {
    fld::TabInfo t;
    t.label = d->display_name();
    t.tooltip = d->path.empty() ? "Not saved yet" : d->path;
    t.modified = d->modified;
    tabs.push_back(t);
  }
  g_tabbar->set_tabs(tabs, std::max(0, doc_index(g_current)));
}

static void sync_menus();
static void show_doc(Doc *d);

static void update_tab_label(Doc *d) {
  if (!d) return;
  refresh_tabbar();
  if (d == current()) {
    std::string title = (d->modified ? "*" : "") + d->display_name() + " – Text Editor";
    g_win->copy_label(title.c_str());
  }
}

static void update_status() {
  Doc *d = current();
  if (!d) { g_status->label(""); return; }
  int pos = d->ed->insert_position();
  int line = d->buf->count_lines(0, pos) + 1;
  int col = pos - d->buf->line_start(pos) + 1;
  char buf[200];
  snprintf(buf, sizeof buf, "Ln %d, Col %d    %s    %s    %s", line, col, syntax::language_name(d->lang),
           S.spaces ? ("Spaces: " + std::to_string(S.tab_width)).c_str() : ("Tab width: " + std::to_string(S.tab_width)).c_str(),
           !d->ed->extra.empty() ? (std::to_string(d->ed->extra.size() + 1) + " carets").c_str()
           : d->buf->selected()  ? "Selection"
                                 : "");
  std::string st = buf;
  // the language server, its problem count and the problem on this line
  std::string server = lspc::name(d->lsp);
  if (!server.empty()) {
    int errs = 0, warns = 0;
    const lsp::Diagnostic *here = nullptr;
    for (auto &dg : d->diags) {
      if (dg.severity == 1) errs++;
      else if (dg.severity == 2) warns++;
      if (!here && dg.line <= line - 1 && dg.end_line >= line - 1 && dg.severity <= 2) here = &dg;
    }
    st += "    " + server;
    if (errs || warns) st += ": " + std::to_string(errs) + (errs == 1 ? " error, " : " errors, ") +
                             std::to_string(warns) + (warns == 1 ? " warning" : " warnings");
    if (here) st += "    " + here->message.substr(0, here->message.find('\n'));
  }
  g_status->copy_label(fld::escape_at(st).c_str());
}

static void editor_menu(Doc *d);

int Editor::handle(int e) {
  if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
    if (Fl::focus() != this) take_focus();
    // a click outside the selection moves the cursor there first
    int pos = xy_to_position(Fl::event_x(), Fl::event_y(), CURSOR_POS);
    int a, b;
    if (!buffer()->selection_position(&a, &b) || pos < a || pos > b) {
      buffer()->unselect();
      insert_position(pos);
    }
    editor_menu(doc);
    return 1;
  }
  if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
    if (Fl::event_state() & FL_ALT) {
      // Alt+click adds a caret
      Caret c = primary_caret(this);
      int pos = xy_to_position(Fl::event_x(), Fl::event_y(), CURSOR_POS);
      if (pos != c.pos) extra.push_back(c);
      buffer()->unselect();
      insert_position(pos);
      if (Fl::focus() != this) take_focus();
      redraw();
      return 1;
    }
    if (!extra.empty()) {
      extra.clear();
      redraw();
    }
  }
  if (e == FL_KEYBOARD && !extra.empty()) {
    if (handle_carets(Fl::event_key(), Fl::event_state())) {
      update_status();
      return 1;
    }
  }
  if (e == FL_KEYBOARD) {
    int k = Fl::event_key();
    int st = Fl::event_state();
    // auto indent
    if ((k == FL_Enter || k == FL_KP_Enter) && !(st & (FL_CTRL | FL_ALT)) && S.auto_indent) {
      int pos = insert_position();
      int ls = buffer()->line_start(pos);
      std::string indent;
      for (int i = ls; i < pos; i++) {
        char c = buffer()->byte_at(i);
        if (c == ' ' || c == '\t') indent += c;
        else break;
      }
      if (buffer()->selected()) buffer()->remove_selection();
      pos = insert_position();
      std::string ins = "\n" + indent;
      buffer()->insert(pos, ins.c_str());
      insert_position(pos + (int)ins.size());
      show_insert_position();
      update_status();
      return 1;
    }
    if (k == FL_Tab && !(st & (FL_CTRL | FL_ALT))) {
      if (buffer()->selected()) {
        // indent / unindent the selected lines
        int a, b;
        buffer()->selection_position(&a, &b);
        a = buffer()->line_start(a);
        if (b > a && buffer()->byte_at(b - 1) == '\n') b--;
        std::string unit = S.spaces ? std::string(S.tab_width, ' ') : "\t";
        std::string out;
        char *sel = buffer()->text_range(a, b);
        std::string text(sel);
        free(sel);
        for (auto &line : fld::split(text, '\n', true)) {
          if (!out.empty()) out += '\n';
          if (st & FL_SHIFT) {
            if (!line.empty() && line[0] == '\t') line.erase(0, 1);
            else {
              size_t n = 0;
              while (n < line.size() && n < (size_t)S.tab_width && line[n] == ' ') n++;
              line.erase(0, n);
            }
            out += line;
          } else out += line.empty() ? line : unit + line;
        }
        buffer()->replace(a, b, out.c_str());
        buffer()->select(a, a + (int)out.size());
        return 1;
      }
      if (st & FL_SHIFT) return 1;
      if (S.spaces) {
        int pos = insert_position();
        int col = pos - buffer()->line_start(pos);
        std::string sp(S.tab_width - col % S.tab_width, ' ');
        insert(sp.c_str());
        return 1;
      }
    }
  }
  int r = Fl_Text_Editor::handle(e);
  if (e == FL_KEYBOARD || e == FL_PUSH || e == FL_DRAG || e == FL_RELEASE) {
    update_status();
    doc->update_brackets();
  }
  return r;
}

// ---- recent files & settings ---------------------------------------------------------

static std::string settings_path() { return fld::fld_config_dir() + "/edit.ini"; }

static void save_settings() {
  fld::Ini cfg;
  cfg.load(settings_path());
  cfg.set_bool("Editor", "LineNumbers", S.line_numbers);
  cfg.set_bool("Editor", "WordWrap", S.wrap);
  cfg.set_bool("Editor", "Highlight", S.highlight);
  cfg.set_bool("Editor", "InsertSpaces", S.spaces);
  cfg.set_bool("Editor", "AutoIndent", S.auto_indent);
  cfg.set_bool("Editor", "MatchBrackets", S.brackets);
  cfg.set_bool("Editor", "Minimap", S.minimap);
  cfg.set_bool("Editor", "LanguageServers", S.lsp);
  cfg.set_bool("Editor", "RestoreSession", S.restore_session);
  cfg.set("Editor", "ColorTheme", S.theme);
  cfg.set_int("Editor", "FontSize", S.font_size);
  cfg.set_int("Editor", "TabWidth", S.tab_width);
  cfg.set("Editor", "Recent", fld::join(S.recent, ";"));
  cfg.save(settings_path());
}

static void load_settings() {
  fld::Ini cfg;
  cfg.load(settings_path());
  S.line_numbers = cfg.get_bool("Editor", "LineNumbers", true);
  S.wrap = cfg.get_bool("Editor", "WordWrap", false);
  S.highlight = cfg.get_bool("Editor", "Highlight", true);
  S.spaces = cfg.get_bool("Editor", "InsertSpaces", false);
  S.auto_indent = cfg.get_bool("Editor", "AutoIndent", true);
  S.brackets = cfg.get_bool("Editor", "MatchBrackets", true);
  S.minimap = cfg.get_bool("Editor", "Minimap", true);
  S.lsp = cfg.get_bool("Editor", "LanguageServers", true);
  S.restore_session = cfg.get_bool("Editor", "RestoreSession", true);
  // [LanguageServers] cpp=clangd --background-index, python=pyright-langserver --stdio, ...
  std::map<std::string, std::string> servers;
  for (auto &k : cfg.keys("LanguageServers")) servers[k] = cfg.get("LanguageServers", k, "");
  lspc::set_command_resolver([servers](const std::string &id) {
    auto it = servers.find(id);
    if (it != servers.end()) return fld::shell_split(it->second);
    return lsp::default_server(id);
  });
  S.theme = cfg.get("Editor", "ColorTheme", "auto");
  S.font_size = cfg.get_int("Editor", "FontSize", fld::mono_size());
  S.tab_width = std::max(1, std::min(16, cfg.get_int("Editor", "TabWidth", 4)));
  S.recent = fld::split(cfg.get("Editor", "Recent", ""), ';');
}

static void rebuild_recent_menu();

static void add_recent(const std::string &p) {
  // Settings > Privacy can turn the list off
  fld::Ini desk;
  desk.load(fld::desktop_config_path());
  if (!desk.get_bool("Privacy", "RememberRecent", true)) return;
  S.recent.erase(std::remove(S.recent.begin(), S.recent.end(), p), S.recent.end());
  S.recent.insert(S.recent.begin(), p);
  if (S.recent.size() > 10) S.recent.resize(10);
  save_settings();
  rebuild_recent_menu();
}

// ---- document management ---------------------------------------------------------------

static void close_doc(Doc *d);

// shows d and hides the other documents
static void show_doc(Doc *d) {
  if (!d) return;
  g_current = d;
  for (auto *o : g_docs) {
    if (o == d) o->show();
    else o->hide();
  }
  update_tab_label(d);
  d->ed->take_focus();
  sync_menus();
  update_status();
  g_area->redraw();
}

static Doc *new_doc() {
  g_area->begin();
  Doc *d = new Doc(g_area->x(), g_area->y(), g_area->w(), g_area->h());
  g_area->end();
  g_docs.push_back(d);
  show_doc(d);
  update_status();
  return d;
}

static Doc *find_doc(const std::string &p) {
  std::string a = fld::abs_path(p);
  for (auto d : g_docs) if (d->path == a) return d;
  return nullptr;
}

static void open_file(const std::string &p) {
  if (Doc *d = find_doc(p)) {
    show_doc(d);
    return;
  }
  Doc *cur = current();
  // reuse an empty, untouched "Untitled" tab
  Doc *d = (cur && cur->path.empty() && !cur->modified && cur->buf->length() == 0) ? cur : new_doc();
  if (!fld::file_exists(p)) {
    // a new file: remember the path, create on save
    d->path = fld::abs_path(p);
    d->lang = syntax::detect(d->path, "");
    d->apply_settings();
    update_tab_label(d);
    return;
  }
  if (d->load(p)) add_recent(d->path);
  show_doc(d);
}

static bool save_as(Doc *d) {
  std::string start = d->path.empty() ? fld::home_dir() + "/" : d->path;
  const char *f = fl_file_chooser("Save As", "*", start.c_str(), 0);
  if (!f) return false;
  if (fld::file_exists(f) && fld::abs_path(f) != d->path &&
      fl_choice("\"%s\" already exists. Replace it?", "Cancel", "Replace", nullptr, fld::basename(f).c_str()) != 1)
    return false;
  if (!d->save(f)) return false;
  add_recent(d->path);
  return true;
}

static bool save(Doc *d) {
  if (d->path.empty()) return save_as(d);
  return d->save(d->path);
}

// Returns false if the user cancelled.
static bool confirm_close(Doc *d) {
  if (!d->modified) return true;
  show_doc(d);
  int c = fl_choice("Save changes to \"%s\" before closing?", "Cancel", "Save", "Don't Save",
                    d->display_name().c_str());
  if (c == 0) return false;
  if (c == 1) return save(d);
  return true;
}

static void close_doc(Doc *d) {
  if (!confirm_close(d)) return;
  int i = doc_index(d);
  if (i < 0) return;
  g_docs.erase(g_docs.begin() + i);
  Fl::remove_timeout(Doc::highlight_later, d);
  g_area->remove(d);
  Fl::delete_widget(d);
  if (g_current == d) g_current = nullptr;
  if (g_docs.empty()) new_doc();
  else if (!g_current) show_doc(g_docs[(size_t)std::min(i, (int)g_docs.size() - 1)]);
  else refresh_tabbar();
}

// ---- session: the open files come back at the next start ----------------------------------

static std::string session_path() { return fld::fld_config_dir() + "/edit-session.ini"; }

static void save_session() {
  fld::Ini ini;
  int n = 0;
  for (auto d : g_docs) {
    if (d->path.empty() || !fld::file_exists(d->path)) continue;
    std::string sec = "File" + std::to_string(n++);
    ini.set(sec, "Path", d->path);
    ini.set_int(sec, "Cursor", d->ed->insert_position());
    ini.set_int(sec, "Top", d->ed->scroll_row());
    if (d == current()) ini.set_int("Session", "Current", n - 1);
  }
  ini.set_int("Session", "Count", n);
  ini.save(session_path());
}

static void open_file(const std::string &p);

static int restore_session() {
  fld::Ini ini;
  if (!ini.load(session_path())) return 0;
  int n = ini.get_int("Session", "Count", 0), cur = ini.get_int("Session", "Current", 0), opened = 0;
  Doc *show = nullptr;
  for (int i = 0; i < n && i < 100; i++) {
    std::string sec = "File" + std::to_string(i), p = ini.get(sec, "Path", "");
    if (p.empty() || !fld::file_exists(p)) continue;
    open_file(p);
    Doc *d = current();
    if (!d || d->path != p) continue;
    d->ed->insert_position(std::max(0, std::min(ini.get_int(sec, "Cursor", 0), d->buf->length())));
    d->ed->scroll(std::max(1, ini.get_int(sec, "Top", 1)), 0);
    if (i == cur) show = d;
    opened++;
  }
  if (show) show_doc(show);
  return opened;
}

static void quit() {
  for (auto d : std::vector<Doc *>(g_docs))
    if (!confirm_close(d)) return;
  for (auto d : g_docs) d->modified = false;
  save_settings();
  if (S.restore_session) save_session();
  lspc::shutdown_all();
  g_win->hide();
}

// ---- language server commands ----------------------------------------------------------

static void status_back(void *) { update_status(); }

static void status_note(const std::string &msg) {
  g_status->copy_label(fld::escape_at(msg).c_str());
  Fl::remove_timeout(status_back);
  Fl::add_timeout(3, status_back);
}

static bool need_server(Doc *d) {
  if (d && d->lsp && !lspc::name(d->lsp).empty()) return true;
  if (!S.lsp) status_note("Language servers are turned off (Options menu)");
  else if (d && d->path.empty()) status_note("Save the file first: language servers work on files");
  else {
    std::vector<std::string> cmd = lsp::default_server(d ? d->lsp_lang() : "");
    status_note(cmd.empty() ? "No language server is known for this language"
                            : "No language server running (is " + cmd[0] + " installed?)");
  }
  return false;
}

static void show_completion(Doc *d, int pos, const std::vector<lsp::CompletionItem> &items) {
  Fl_Text_Buffer *b = d->buf;
  int ws = pos;
  while (ws > 0) {
    char c = b->byte_at(ws - 1);
    if (!(isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80)) break;
    ws--;
  }
  char *t = b->text_range(ws, pos);
  std::string prefix(t);
  free(t);
  std::vector<const lsp::CompletionItem *> shown;
  for (auto &it : items) {
    if (!prefix.empty() && strncasecmp(it.insert.c_str(), prefix.c_str(), prefix.size()) != 0 &&
        strncasecmp(it.label.c_str(), prefix.c_str(), prefix.size()) != 0)
      continue;
    shown.push_back(&it);
    if (shown.size() >= 40) break;
  }
  if (shown.empty()) {
    status_note("No completions");
    return;
  }
  std::vector<std::string> labels;
  for (auto *it : shown) {
    std::string l;
    for (char c : it->label) {
      if (c == '&' || c == '@') l += c;
      l += c;
    }
    if (!it->detail.empty()) l += "   " + it->detail.substr(0, 40);
    labels.push_back(l);
  }
  std::vector<Fl_Menu_Item> menu;
  for (size_t i = 0; i < shown.size(); i++) menu.push_back({labels[i].c_str(), 0, nullptr, (void *)i});
  menu.push_back({nullptr});
  int X, Y;
  if (!d->ed->xy_of(pos, X, Y)) {
    X = d->ed->x() + 40;
    Y = d->ed->y() + 20;
  }
  const Fl_Menu_Item *r = menu[0].popup(X, Y + d->ed->line_height());
  if (!r) return;
  const lsp::CompletionItem *it = shown[(size_t)(long)r->user_data()];
  b->replace(ws, pos, it->insert.c_str());
  d->ed->insert_position(ws + (int)it->insert.size());
  d->ed->show_insert_position();
}

// Ctrl+Space
static void complete() {
  Doc *d = current();
  if (!need_server(d)) return;
  d->lsp_flush();
  int pos = d->ed->insert_position(), line, col;
  d->lsp_position(pos, line, col);
  lspc::complete(d->lsp, d->path, line, col, [d, pos](const std::vector<lsp::CompletionItem> &items) {
    // the answer is late: only when nothing moved
    if (doc_index(d) < 0 || current() != d || d->ed->insert_position() != pos) return;
    show_completion(d, pos, items);
  });
}

static void go_to(Doc *d, int line, int col16) {
  int p = d->pos_of(line, col16);
  d->buf->unselect();
  d->ed->insert_position(p);
  d->ed->show_insert_position();
  update_status();
}

// F12
static void go_to_definition() {
  Doc *d = current();
  if (!need_server(d)) return;
  d->lsp_flush();
  int line, col;
  d->lsp_position(d->ed->insert_position(), line, col);
  lspc::definition(d->lsp, d->path, line, col, [](const std::vector<lsp::Location> &locs) {
    if (locs.empty()) {
      status_note("No definition found");
      return;
    }
    const lsp::Location &l = locs[0];
    open_file(l.path);
    Doc *t = current();
    if (t && t->path == fld::abs_path(l.path)) go_to(t, l.line, l.col);
  });
}

// F8 and Shift+F8
static void next_problem(bool back) {
  Doc *d = current();
  if (!d || d->diags.empty()) {
    status_note("No problems");
    return;
  }
  int pos = d->ed->insert_position();
  const lsp::Diagnostic *pick = nullptr;
  if (!back) {
    for (auto &dg : d->diags)
      if (d->pos_of(dg.line, dg.col) > pos) { pick = &dg; break; }
    if (!pick) pick = &d->diags.front();
  } else {
    for (auto it = d->diags.rbegin(); it != d->diags.rend(); ++it)
      if (d->pos_of(it->line, it->col) < pos) { pick = &*it; break; }
    if (!pick) pick = &d->diags.back();
  }
  go_to(d, pick->line, pick->col);
}

// ---- find / replace -------------------------------------------------------------------------

struct FindDialog {
  Fl_Double_Window *win = nullptr;
  Fl_Input *find = nullptr, *repl = nullptr;
  Fl_Check_Button *match_case = nullptr, *wrap = nullptr;
  Fl_Box *msg = nullptr;
} F;

static bool find_next(bool backwards = false) {
  Doc *d = current();
  if (!d || !F.find || !*F.find->value()) return false;
  const char *needle = F.find->value();
  bool mc = F.match_case->value();
  int start = d->ed->insert_position(), found;
  bool ok;
  if (backwards) {
    int a, b;
    if (d->buf->selection_position(&a, &b)) start = a;
    ok = start > 0 && d->buf->search_backward(start - 1, needle, &found, mc);
    if (!ok && F.wrap->value()) ok = d->buf->search_backward(d->buf->length(), needle, &found, mc);
  } else {
    ok = d->buf->search_forward(start, needle, &found, mc);
    if (!ok && F.wrap->value()) ok = d->buf->search_forward(0, needle, &found, mc);
  }
  if (!ok) {
    F.msg->label("Not found");
    fl_beep();
    return false;
  }
  F.msg->label("");
  int len = (int)strlen(needle);
  d->buf->select(found, found + len);
  d->ed->insert_position(backwards ? found : found + len);
  d->ed->show_insert_position();
  update_status();
  return true;
}

static void replace_one() {
  Doc *d = current();
  if (!d || !*F.find->value()) return;
  int a, b;
  if (d->buf->selection_position(&a, &b)) {
    char *sel = d->buf->selection_text();
    bool match = F.match_case->value() ? strcmp(sel, F.find->value()) == 0 : strcasecmp(sel, F.find->value()) == 0;
    free(sel);
    if (match) {
      d->buf->replace(a, b, F.repl->value());
      d->ed->insert_position(a + (int)strlen(F.repl->value()));
    }
  }
  find_next();
}

static void replace_all() {
  Doc *d = current();
  if (!d || !*F.find->value()) return;
  const char *needle = F.find->value(), *rep = F.repl->value();
  int pos = 0, found, count = 0, nl = (int)strlen(needle), rl = (int)strlen(rep);
  while (d->buf->search_forward(pos, needle, &found, F.match_case->value())) {
    d->buf->replace(found, found + nl, rep);
    pos = found + rl;
    count++;
  }
  static char m[64];
  snprintf(m, sizeof m, "Replaced %d occurrence%s", count, count == 1 ? "" : "s");
  F.msg->label(m);
}

static void show_find(bool replace) {
  if (!F.win) {
    F.win = new Fl_Double_Window(440, 160, "Find and Replace");
    F.find = new Fl_Input(90, 12, 230, 28, "Find:");
    F.repl = new Fl_Input(90, 46, 230, 28, "Replace:");
    F.match_case = new Fl_Check_Button(90, 80, 120, 24, "Match case");
    F.wrap = new Fl_Check_Button(210, 80, 120, 24, "Wrap around");
    F.wrap->value(1);
    Fl_Return_Button *next = new Fl_Return_Button(330, 12, 100, 28, "Next");
    next->callback([](Fl_Widget *, void *) { find_next(); });
    Fl_Button *prev = new Fl_Button(330, 46, 100, 28, "Previous");
    prev->callback([](Fl_Widget *, void *) { find_next(true); });
    Fl_Button *r1 = new Fl_Button(220, 120, 100, 28, "Replace");
    r1->callback([](Fl_Widget *, void *) { replace_one(); });
    Fl_Button *ra = new Fl_Button(330, 120, 100, 28, "Replace All");
    ra->callback([](Fl_Widget *, void *) { replace_all(); });
    F.msg = new Fl_Box(10, 120, 200, 28);
    F.msg->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
    F.msg->labelsize(12);
    F.win->end();
    F.win->set_non_modal();
  }
  Doc *d = current();
  if (d && d->buf->selected()) {
    char *sel = d->buf->selection_text();
    if (!strchr(sel, '\n')) F.find->value(sel);
    free(sel);
  }
  F.win->show();
  (replace && *F.find->value() ? F.repl : F.find)->take_focus();
}

static void goto_line() {
  Doc *d = current();
  if (!d) return;
  const char *v = fl_input("Go to line:", "");
  if (!v) return;
  int line = atoi(v);
  if (line < 1) return;
  int pos = d->buf->skip_lines(0, line - 1);
  d->ed->insert_position(pos);
  d->ed->show_insert_position();
  d->ed->take_focus();
  update_status();
}

// ---- menus ------------------------------------------------------------------------------------

static void apply_all() {
  for (auto d : g_docs) d->apply_settings();
  save_settings();
  update_status();
}

static void menu_cb(Fl_Widget *, void *v) {
  long id = (long)v;
  Doc *d = current();
  switch (id) {
    case 1: new_doc(); break;
    case 2: {
      std::string dir = d && !d->path.empty() ? fld::dirname(d->path) : fld::home_dir();
      Fl_File_Chooser fc(dir.c_str(), "*", Fl_File_Chooser::MULTI, "Open Files");
      fc.show();
      while (fc.shown()) Fl::wait();
      for (int i = 1; i <= fc.count(); i++) if (fc.value(i)) open_file(fc.value(i));
      break;
    }
    case 3: if (d) save(d); break;
    case 4: if (d) save_as(d); break;
    case 5:
      if (d && !d->path.empty() &&
          (!d->modified || fl_choice("Discard your changes and reload?", "Cancel", "Reload", nullptr) == 1))
        d->load(d->path);
      break;
    case 6: if (d) close_doc(d); break;
    case 7: quit(); break;
    case 10: if (d) { int cp; if (d->buf->undo(&cp)) d->ed->insert_position(cp); } break;
    case 11: if (d) { int cp; if (d->buf->redo(&cp)) d->ed->insert_position(cp); } break;
    case 12: if (d) Fl_Text_Editor::kf_cut(0, d->ed); break;
    case 13: if (d) Fl_Text_Editor::kf_copy(0, d->ed); break;
    case 14: if (d) Fl_Text_Editor::kf_paste(0, d->ed); break;
    case 15: if (d) Fl_Text_Editor::kf_select_all(0, d->ed); break;
    case 16: show_find(false); break;
    case 17: if (!find_next()) show_find(false); break;
    case 18: if (!find_next(true)) show_find(false); break;
    case 19: show_find(true); break;
    case 20: goto_line(); break;
    case 30: S.line_numbers = !S.line_numbers; apply_all(); break;
    case 31: S.wrap = !S.wrap; apply_all(); break;
    case 32: S.highlight = !S.highlight; apply_all(); break;
    case 33: S.font_size = std::min(48, S.font_size + 1); apply_all(); break;
    case 34: S.font_size = std::max(8, S.font_size - 1); apply_all(); break;
    case 35: S.font_size = fld::mono_size(); apply_all(); break;
    case 36: S.spaces = !S.spaces; apply_all(); break;
    case 37: S.auto_indent = !S.auto_indent; apply_all(); break;
    case 41: S.brackets = !S.brackets; apply_all(); break;
    case 38: case 39: case 40: S.tab_width = id == 38 ? 2 : id == 39 ? 4 : 8; apply_all(); break;
    case 21: if (d) d->ed->add_next_occurrence(); break;
    case 22: if (d) d->ed->add_caret_vertical(-1); break;
    case 23: if (d) d->ed->add_caret_vertical(1); break;
    case 24: complete(); break;
    case 25: go_to_definition(); break;
    case 26: next_problem(false); break;
    case 27: next_problem(true); break;
    case 42: S.minimap = !S.minimap; apply_all(); break;
    case 43:
      S.lsp = !S.lsp;
      save_settings();
      if (!S.lsp) {
        lspc::shutdown_all();
        for (auto x : g_docs) {
          x->lsp = nullptr;
          x->set_diagnostics({});
        }
      } else
        for (auto x : g_docs) x->lsp_reopen();
      update_status();
      break;
    case 44: S.restore_session = !S.restore_session; save_settings(); break;
    case 50: fld::about_dialog("Text Editor", "fl-edit", "A text and code editor built with FLTK."); break;
    case 60: if (d) { if (!d->path.empty()) fld::spawn({"fl-term", "--workdir", fld::dirname(d->path)}); else fld::spawn({"fl-term"}); } break;
  }
  if (d && id >= 10 && id < 20) update_status();
}

static void recent_cb(Fl_Widget *, void *v) { open_file((const char *)v); }

static void sync_menus();

static void language_cb(Fl_Widget *, void *v) {
  Doc *d = current();
  if (!d) return;
  long l = (long)v;
  if (l < 0) {
    d->lang_forced = false;
    char *t = d->buf->text();
    std::string s(t);
    free(t);
    d->lang = syntax::detect(d->path, s.substr(0, s.find('\n')));
  } else {
    d->lang_forced = true;
    d->lang = (syntax::Language)l;
  }
  d->apply_settings();
  d->lsp_reopen();
  sync_menus();
  update_status();
}

static std::vector<std::string> g_theme_ids;

static void theme_cb(Fl_Widget *, void *v) {
  long i = (long)v;
  S.theme = i < 0 ? "auto" : g_theme_ids[(size_t)i];
  apply_all();
  sync_menus();
}

// radio items follow the current document / settings
static void sync_menus() {
  Doc *d = current();
  for (int i = 0; i < g_menu->size(); i++) {
    Fl_Menu_Item *m = (Fl_Menu_Item *)&g_menu->menu()[i];
    if (!m->label()) continue;
    bool on = false;
    if (m->callback() == language_cb) {
      long l = (long)m->user_data();
      on = d && (l < 0 ? !d->lang_forced : d->lang_forced && d->lang == l);
    } else if (m->callback() == theme_cb) {
      long t = (long)m->user_data();
      on = t < 0 ? S.theme == "auto" : S.theme == g_theme_ids[(size_t)t];
    } else continue;
    if (on) m->set(); else m->clear();
  }
}

static void rebuild_recent_menu() {
  int idx = g_menu->find_index("&File/Open &Recent");
  if (idx < 0) return;
  g_menu->clear_submenu(idx);
  if (S.recent.empty()) {
    g_menu->add("&File/Open &Recent/(empty)", 0, nullptr, nullptr, FL_MENU_INACTIVE);
    return;
  }
  for (auto &r : S.recent) {
    std::string label = "&File/Open &Recent/" + fld::tilde(r);
    // escape menu path separators and shortcuts
    std::string esc;
    for (size_t i = std::string("&File/Open &Recent/").size(); i < label.size(); i++) {
      char c = label[i];
      if (c == '/' || c == '\\' || c == '&' || c == '_' || c == '@') esc += '\\';
      esc += c;
    }
    g_menu->add(("&File/Open &Recent/" + esc).c_str(), 0, recent_cb, (void *)strdup(r.c_str()));
  }
}

// ---- tab and editor menus ---------------------------------------------------------------

static void later(std::function<void()> f) {
  auto *p = new std::function<void()>(std::move(f));
  Fl::add_timeout(0, [](void *v) {
    auto *fn = (std::function<void()> *)v;
    (*fn)();
    delete fn;
  }, p);
}

static void close_others(Doc *keep) {
  for (auto *d : std::vector<Doc *>(g_docs))
    if (d != keep && !confirm_close(d)) return;
  for (auto *d : std::vector<Doc *>(g_docs))
    if (d != keep) {
      d->modified = false; // confirmed above
      close_doc(d);
    }
  show_doc(keep);
}

static void close_right(Doc *from) {
  int i = doc_index(from);
  if (i < 0) return;
  std::vector<Doc *> right(g_docs.begin() + i + 1, g_docs.end());
  for (auto *d : right)
    if (!confirm_close(d)) return;
  for (auto *d : right) {
    d->modified = false;
    close_doc(d);
  }
  show_doc(from);
}

// opens the file in a new fl-edit window and closes its tab here
static void doc_to_window(Doc *d) {
  if (doc_index(d) < 0) return;
  if (d->path.empty() || d->modified) {
    if (fl_choice("\"%s\" has to be saved before it can move to a new window.", "Cancel", "Save", nullptr,
                  d->display_name().c_str()) != 1 || !save(d))
      return;
  }
  if (fld::spawn({"fl-edit", d->path})) close_doc(d);
}

// ---- moving tabs to other fl-edit windows (the text goes along, saved or not)

static std::string export_doc(int i) {
  if (i < 0 || i >= (int)g_docs.size()) return "";
  Doc *d = g_docs[(size_t)i];
  fld::Json j = fld::Json::object();
  j.set("path", d->path);
  j.set("modified", d->modified);
  if (d->modified || d->path.empty()) {
    char *t = d->buf->text();
    j.set("text", std::string(t));
    free(t);
  }
  j.set("cursor", d->ed->insert_position());
  if (d->lang_forced) j.set("lang", (int)d->lang);
  return j.dump();
}

// opens an exported tab at index at
static bool import_doc(const std::string &state, int at) {
  fld::Json j;
  if (!fld::Json::parse(state, j)) return false;
  std::string path = j["path"].as_string();
  bool modified = j["modified"].as_bool();
  Doc *d = nullptr;
  if (!path.empty() && !modified && (d = find_doc(path))) {
    show_doc(d); // already open here
  } else {
    Doc *cur = current();
    d = (cur && cur->path.empty() && !cur->modified && cur->buf->length() == 0) ? cur : new_doc();
    if (j.has("text")) {
      std::string text = j["text"].as_string();
      d->loading = true;
      d->buf->text(text.c_str());
      d->loading = false;
      d->buf->canUndo(0);
      d->buf->canUndo(1);
      d->path = path.empty() ? "" : fld::abs_path(path);
      if (j.has("lang")) {
        d->lang = (syntax::Language)(int)j["lang"].as_number();
        d->lang_forced = true;
      } else d->lang = syntax::detect(d->path, text.substr(0, text.find('\n')));
      d->modified = modified;
      d->apply_settings();
      update_tab_label(d);
    } else if (!d->load(path)) {
      return false;
    }
    show_doc(d);
  }
  int cursor = (int)j["cursor"].as_number();
  if (cursor >= 0 && cursor <= d->buf->length()) {
    d->ed->insert_position(cursor);
    d->ed->show_insert_position();
  }
  int from = doc_index(d);
  int to = std::max(0, std::min((int)g_docs.size() - 1, at));
  if (from >= 0 && from != to) fld::move_element(g_docs, from, to);
  refresh_tabbar();
  g_win->show();
  return true;
}

// the tab's text went to another window: close it without asking
static void doc_moved_away(int i) {
  if (i < 0 || i >= (int)g_docs.size()) return;
  Doc *d = g_docs[(size_t)i];
  d->modified = false;
  if (g_docs.size() == 1) {
    save_settings();
    g_win->hide();
    return;
  }
  close_doc(d);
}

static void reload_doc(Doc *d) {
  if (d && !d->path.empty() &&
      (!d->modified || fl_choice("Discard your changes and reload?", "Cancel", "Reload", nullptr) == 1))
    d->load(d->path);
}

static void tab_menu(int i) {
  fld::Menu m;
  if (i < 0 || i >= (int)g_docs.size()) {
    m.item("New", [] { new_doc(); }, 0, FL_CTRL + 'n');
    m.popup();
    return;
  }
  Doc *d = g_docs[(size_t)i];
  if (d != current()) show_doc(d);
  int n = (int)g_docs.size();
  bool saved = !d->path.empty();
  m.item("New", [] { new_doc(); }, 0, FL_CTRL + 'n').divider();
  m.item("Save", [d] { later([d] { if (doc_index(d) >= 0) save(d); }); }, 0, FL_CTRL + 's');
  m.item("Save As…", [d] { later([d] { if (doc_index(d) >= 0) save_as(d); }); }, 0, FL_CTRL + FL_SHIFT + 's');
  m.item("Reload", [d] { later([d] { if (doc_index(d) >= 0) reload_doc(d); }); }, saved ? 0 : FL_MENU_INACTIVE);
  m.divider();
  m.item("Copy Path", [d] { Fl::copy(d->path.c_str(), (int)d->path.size(), 1); }, saved ? 0 : FL_MENU_INACTIVE);
  m.item("Open Containing Folder", [d] { fld::spawn({"fl-files", fld::dirname(d->path)}); }, saved ? 0 : FL_MENU_INACTIVE);
  m.item("Open Terminal Here", [d] {
    if (!d->path.empty()) fld::spawn({"fl-term", "--workdir", fld::dirname(d->path)});
    else fld::spawn({"fl-term"});
  });
  m.divider();
  m.item("Move Tab Left", [i] { fld::move_element(g_docs, i, i - 1); refresh_tabbar(); }, i > 0 ? 0 : FL_MENU_INACTIVE);
  m.item("Move Tab Right", [i] { fld::move_element(g_docs, i, i + 1); refresh_tabbar(); },
         i + 1 < n ? 0 : FL_MENU_INACTIVE);
  m.item("Move to New Window", [d] { later([d] { doc_to_window(d); }); }, n > 1 ? 0 : FL_MENU_INACTIVE);
  m.divider();
  m.item("Close", [d] { later([d] { if (doc_index(d) >= 0) close_doc(d); }); }, 0, FL_CTRL + 'w');
  m.item("Close Others", [d] { later([d] { if (doc_index(d) >= 0) close_others(d); }); }, n > 1 ? 0 : FL_MENU_INACTIVE);
  m.item("Close Tabs to the Right", [d] { later([d] { if (doc_index(d) >= 0) close_right(d); }); },
         i + 1 < n ? 0 : FL_MENU_INACTIVE);
  m.popup();
}

// right click in the text
static void editor_menu(Doc *d) {
  Fl_Text_Buffer *b = d->buf;
  bool sel = b->selected();
  auto run = [](long id) { return [id] { menu_cb(nullptr, (void *)id); }; };
  fld::Menu m;
  m.item("Undo", run(10), b->can_undo() ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'z');
  m.item("Redo", run(11), b->can_redo() ? 0 : FL_MENU_INACTIVE, FL_CTRL + FL_SHIFT + 'z').divider();
  m.item("Cut", run(12), sel ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'x');
  m.item("Copy", run(13), sel ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'c');
  m.item("Paste", run(14), 0, FL_CTRL + 'v');
  m.item("Delete", [d] { Fl_Text_Editor::kf_delete(0, d->ed); }, sel ? 0 : FL_MENU_INACTIVE).divider();
  m.item("Select All", run(15), 0, FL_CTRL + 'a').divider();
  m.item("Find…", [run] { later(run(16)); }, 0, FL_CTRL + 'f');
  m.item("Replace…", [run] { later(run(19)); }, 0, FL_CTRL + 'h');
  m.item("Go to Line…", [run] { later(run(20)); }, 0, FL_CTRL + 'g');
  m.popup();
  update_status();
}

class EditorWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  int handle(int e) override {
    switch (e) {
      case FL_DND_ENTER: case FL_DND_DRAG: case FL_DND_RELEASE: return 1;
      case FL_PASTE:
        if (Fl::belowmouse() == this || !Fl::focus()) {
          for (auto &line : fld::split(std::string(Fl::event_text(), Fl::event_length()), '\n')) {
            std::string l = fld::trim(line);
            if (fld::starts_with(l, "file://")) l = fld::uri_decode(l.substr(7));
            if (!l.empty() && fld::file_exists(l)) open_file(l);
          }
          return 1;
        }
        break;
    }
    return Fl_Double_Window::handle(e);
  }
};

int main(int argc, char **argv) {
  fld::init_app("fl-edit", argc, argv);
  load_settings();
  make_styles();
  const int W = 900, H = 640, MH = 28, SH = 24;
  g_win = new EditorWindow(W, H, "Text Editor");
  g_menu = new Fl_Menu_Bar(0, 0, W, MH);
  g_menu->box(FL_FLAT_BOX);
  g_menu->add("&File/&New", FL_CTRL + 'n', menu_cb, (void *)1);
  g_menu->add("&File/&Open…", FL_CTRL + 'o', menu_cb, (void *)2);
  g_menu->add("&File/Open &Recent", 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER);
  g_menu->add("&File/&Save", FL_CTRL + 's', menu_cb, (void *)3);
  g_menu->add("&File/Save &As…", FL_CTRL + FL_SHIFT + 's', menu_cb, (void *)4);
  g_menu->add("&File/Re&load", 0, menu_cb, (void *)5, FL_MENU_DIVIDER);
  g_menu->add("&File/Open &Terminal Here", 0, menu_cb, (void *)60, FL_MENU_DIVIDER);
  g_menu->add("&File/&Close Tab", FL_CTRL + 'w', menu_cb, (void *)6);
  g_menu->add("&File/&Quit", FL_CTRL + 'q', menu_cb, (void *)7);
  g_menu->add("&Edit/&Undo", FL_CTRL + 'z', menu_cb, (void *)10);
  g_menu->add("&Edit/&Redo", FL_CTRL + FL_SHIFT + 'z', menu_cb, (void *)11, FL_MENU_DIVIDER);
  g_menu->add("&Edit/Cu&t", FL_CTRL + 'x', menu_cb, (void *)12);
  g_menu->add("&Edit/&Copy", FL_CTRL + 'c', menu_cb, (void *)13);
  g_menu->add("&Edit/&Paste", FL_CTRL + 'v', menu_cb, (void *)14);
  g_menu->add("&Edit/Select &All", FL_CTRL + 'a', menu_cb, (void *)15, FL_MENU_DIVIDER);
  g_menu->add("&Edit/&Find…", FL_CTRL + 'f', menu_cb, (void *)16);
  g_menu->add("&Edit/Find &Next", FL_F + 3, menu_cb, (void *)17);
  g_menu->add("&Edit/Find Pre&vious", FL_SHIFT + FL_F + 3, menu_cb, (void *)18);
  g_menu->add("&Edit/R&eplace…", FL_CTRL + 'h', menu_cb, (void *)19);
  g_menu->add("&Edit/&Go to Line…", FL_CTRL + 'g', menu_cb, (void *)20, FL_MENU_DIVIDER);
  g_menu->add("&Edit/Add Next &Occurrence", FL_CTRL + 'd', menu_cb, (void *)21);
  g_menu->add("&Edit/Add Caret A&bove", FL_CTRL + FL_ALT + FL_Up, menu_cb, (void *)22);
  g_menu->add("&Edit/Add Caret Be&low", FL_CTRL + FL_ALT + FL_Down, menu_cb, (void *)23);
  g_menu->add("&Code/&Complete", FL_CTRL + ' ', menu_cb, (void *)24);
  g_menu->add("&Code/Go to &Definition", FL_F + 12, menu_cb, (void *)25, FL_MENU_DIVIDER);
  g_menu->add("&Code/&Next Problem", FL_F + 8, menu_cb, (void *)26);
  g_menu->add("&Code/&Previous Problem", FL_SHIFT + FL_F + 8, menu_cb, (void *)27, FL_MENU_DIVIDER);
  g_menu->add("&Code/&Language Servers", 0, menu_cb, (void *)43, FL_MENU_TOGGLE | (S.lsp ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Line Numbers", 0, menu_cb, (void *)30, FL_MENU_TOGGLE | (S.line_numbers ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Word Wrap", FL_ALT + 'z', menu_cb, (void *)31, FL_MENU_TOGGLE | (S.wrap ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Syntax Highlighting", 0, menu_cb, (void *)32, FL_MENU_TOGGLE | (S.highlight ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Highlight Matching Brackets", 0, menu_cb, (void *)41, FL_MENU_TOGGLE | (S.brackets ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Minimap", 0, menu_cb, (void *)42, FL_MENU_TOGGLE | (S.minimap ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Language/&Automatic", 0, language_cb, (void *)-1L, FL_MENU_RADIO | FL_MENU_DIVIDER);
  for (auto l : syntax::all_languages()) {
    std::string label = std::string("&View/&Language/") + syntax::language_name(l);
    std::string esc;
    for (char c : label.substr(16)) {
      if (c == '/' || c == '\\' || c == '&' || c == '_' || c == '@') esc += '\\';
      esc += c;
    }
    g_menu->add((label.substr(0, 16) + esc).c_str(), 0, language_cb, (void *)(long)l, FL_MENU_RADIO);
  }
  g_menu->add("&View/&Color Theme/&Automatic", 0, theme_cb, (void *)-1L, FL_MENU_RADIO | FL_MENU_DIVIDER);
  for (size_t i = 0; i < syntax::themes().size(); i++) {
    g_theme_ids.push_back(syntax::themes()[i].id);
    g_menu->add((std::string("&View/&Color Theme/") + syntax::themes()[i].name).c_str(), 0, theme_cb, (void *)(long)i,
                FL_MENU_RADIO | (i + 1 == syntax::themes().size() ? FL_MENU_DIVIDER : 0));
  }
  g_menu->add("&View/Zoom &In", FL_CTRL + '=', menu_cb, (void *)33);
  g_menu->add("&View/Zoom &Out", FL_CTRL + '-', menu_cb, (void *)34);
  g_menu->add("&View/&Normal Size", FL_CTRL + '0', menu_cb, (void *)35);
  g_menu->add("&Options/Insert &Spaces for Tab", 0, menu_cb, (void *)36, FL_MENU_TOGGLE | (S.spaces ? FL_MENU_VALUE : 0));
  g_menu->add("&Options/&Auto Indent", 0, menu_cb, (void *)37, FL_MENU_TOGGLE | (S.auto_indent ? FL_MENU_VALUE : 0));
  g_menu->add("&Options/&Reopen Files at Start", 0, menu_cb, (void *)44,
              FL_MENU_TOGGLE | (S.restore_session ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  g_menu->add("&Options/Tab Width/2", 0, menu_cb, (void *)38, FL_MENU_RADIO | (S.tab_width == 2 ? FL_MENU_VALUE : 0));
  g_menu->add("&Options/Tab Width/4", 0, menu_cb, (void *)39, FL_MENU_RADIO | (S.tab_width == 4 ? FL_MENU_VALUE : 0));
  g_menu->add("&Options/Tab Width/8", 0, menu_cb, (void *)40, FL_MENU_RADIO | (S.tab_width == 8 ? FL_MENU_VALUE : 0));
  g_menu->add("&Help/&About", 0, menu_cb, (void *)50);
  rebuild_recent_menu();

  const int TH = fld::TabBar::preferred_height();
  g_tabbar = new fld::TabBar(0, MH, W, TH);
  g_tabbar->on_select = [](int i) {
    if (i >= 0 && i < (int)g_docs.size()) show_doc(g_docs[(size_t)i]);
  };
  g_tabbar->on_close = [](int i) {
    if (i >= 0 && i < (int)g_docs.size()) later([d = g_docs[(size_t)i]] { if (doc_index(d) >= 0) close_doc(d); });
  };
  g_tabbar->on_new = [] { new_doc(); };
  g_tabbar->on_move = [](int from, int to) {
    fld::move_element(g_docs, from, to);
    refresh_tabbar();
  };
  g_tabbar->on_detach = [](int i) {
    if (i >= 0 && i < (int)g_docs.size()) later([d = g_docs[(size_t)i]] { doc_to_window(d); });
  };
  g_tabbar->on_menu = [](int i) { tab_menu(i); };
  g_tabbar->app_id = "fl-edit";
  g_tabbar->on_export = [](int i) { return export_doc(i); };
  g_tabbar->on_import = [](const std::string &state, int at, bool) { return import_doc(state, at); };
  g_tabbar->on_moved_away = [](int i) { later([i] { doc_moved_away(i); }); };
  g_tabbar->on_tear_off = [](int i, const std::string &file) {
    if (fld::spawn({fld::self_program("fl-edit"), "--adopt-tab", file})) later([i] { doc_moved_away(i); });
    else fld::discard_tab_file(file);
  };
  g_area = new Fl_Group(0, MH + TH, W, H - MH - TH - SH);
  g_area->box(FL_NO_BOX);
  g_area->end();
  g_status = new Fl_Box(0, H - SH, W, SH);
  g_status->box(FL_FLAT_BOX);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->resizable(g_area);
  g_win->end();
  g_win->size_range(400, 300);
  g_win->callback([](Fl_Widget *, void *) {
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return; // don't close on Esc
    quit();
  });
  fld::set_window_icon(g_win, "fl-edit");

  int opened = 0;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") { printf("Usage: fl-edit [files...]\n"); return 0; }
    if (a == "--adopt-tab" && i + 1 < argc) {
      // a tab dragged out of another window
      std::string state;
      if (fld::claim_tab_file(argv[++i], "fl-edit", state) && import_doc(state, (int)g_docs.size())) opened++;
      continue;
    }
    if (a[0] == '-') continue;
    if (fld::starts_with(a, "file://")) a = fld::uri_decode(a.substr(7));
    if (opened == 0 && g_docs.empty()) new_doc();
    open_file(a);
    opened++;
  }
  lspc::set_diagnostics_handler([](const std::string &path, const std::vector<lsp::Diagnostic> &dg) {
    if (Doc *d = find_doc(path)) {
      d->set_diagnostics(dg);
      if (d == current()) update_status();
    }
  });
  if (opened == 0 && S.restore_session) {
    new_doc();
    restore_session();
  }
  if (g_docs.empty()) new_doc();
  sync_menus();
  g_win->show(); // not show(argc, argv): it resets the theme colors
  current()->ed->take_focus();
  update_status();
  return Fl::run();
}
