// fl-edit: what its units share. main.cpp has the settings, the document
// management, the menus and the window; doc.cpp the editor widget, carets,
// minimap and status; lsp_commands.cpp the language server commands;
// find.cpp find, replace and go to line.
#pragma once
#include "fld/window_move.h"
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

#include "fld/git_status.h"
#include "fld/ini.h"
#include "fld/menus.h"
#include "fld/tab_bar.h"
#include "fld/tab_move.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "fld/window_size.h"
#include "lsp.h"
#include "syntax.h"
#ifdef FLD_HAVE_SPELL
#include "spell.h"
#endif

struct Settings {
  bool line_numbers = true, wrap = false, highlight = true, spaces = false, auto_indent = true;
  bool brackets = true, minimap = true, lsp = true, restore_session = true, spell = true, git_gutter = true;
  int font_size = 14, tab_width = 4;
  std::string theme = "auto";
  std::vector<std::string> recent;
};
extern Settings S;  // main.cpp
class Doc;

// ---- defined in one unit, used in others
extern Fl_Double_Window *g_win;
extern fld::TabBar *g_tabbar;
extern Fl_Box *g_status;
static const int MAX_HIGHLIGHT = 4 << 20; // skip highlighting huge files
bool find_next(bool backwards = false);
static const int NSTYLES = syntax::STYLE_COUNT * 3;
extern Fl_Text_Display::Style_Table_Entry g_styles[NSTYLES];
Fl_Color rgb(unsigned v);
const syntax::Theme &cur_theme();
void make_styles();
#ifdef FLD_HAVE_SPELL
spell::Checker *checker();
#endif
bool spell_on();
extern std::vector<Doc *> g_docs;
extern Doc *g_current;
Doc *current();
void show_find(bool replace);
void complete();
void go_to_definition();
void goto_line();
void format_document();
void rename_symbol();
void show_doc(Doc *d);
void open_file(const std::string &p);
void next_problem(bool back);
bool save(Doc *d);
int doc_index(Doc *d);
void refresh_tabbar();
void update_tab_label(Doc *d);
void update_status();
void sync_menus();
void editor_menu(Doc *d);


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
  void draw_git_marks();
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
  std::vector<fld::git::Hunk> git_hunks; // the saved file against git (gutter marks)
  bool modified = false;
  bool loading = false;
  bool is_new = false;             // a path that does not exist yet: created on save
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
  bool styled() const { return (S.highlight || S.brackets || spell_on()) && buf->length() < MAX_HIGHLIGHT; }
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
    overlay_spelling();
    overlay_diagnostics();
    style->text(shown_styles.c_str());
    ed->redisplay_range(0, buf->length());
    update_brackets();
  }

  // the text a language puts prose in: all of plain text, comments and strings in code
  bool spell_style(char st) const {
    syntax::Language l = effective_lang();
    if (l == syntax::NONE) return true;
    if (l == syntax::MARKDOWN) return st == syntax::PLAIN || st == syntax::HEADING;
    return st == syntax::COMMENT || st == syntax::STRING;
  }

  // underlines misspelt words in shown_styles (the warning underline: LSP
  // errors keep the red one)
  void overlay_spelling() {
#ifdef FLD_HAVE_SPELL
    if (!spell_on() || lexed.size() > (2u << 20) || lexed.size() != shown_styles.size()) return;
    spell::Checker *c = checker();
    size_t i = 0, n = lexed.size();
    while (i < n) {
      if (!spell_style(lexed_styles[i])) {
        i++;
        continue;
      }
      size_t a = i;
      char st = lexed_styles[i];
      while (i < n && lexed_styles[i] == st) i++;
      for (const spell::Word &w : spell::words(lexed, a, i)) {
        if (c->check(lexed.substr(w.start, w.len))) continue;
        for (size_t p = w.start; p < w.start + w.len; p++) {
          int s0 = (shown_styles[p] - 'A') % syntax::STYLE_COUNT;
          shown_styles[p] = (char)('A' + s0 + 2 * syntax::STYLE_COUNT);
        }
      }
    }
#endif
  }

  // the misspelt word at pos: its range, or false
  bool misspelt_at(int pos, int &a, int &b) const {
#ifdef FLD_HAVE_SPELL
    if (!spell_on() || dirty || lexed.size() != (size_t)buf->length() || pos < 0 || pos > buf->length()) return false;
    int ls = buf->line_start(pos), le = buf->line_end(pos);
    for (const spell::Word &w : spell::words(lexed, (size_t)ls, (size_t)le)) {
      if (pos < (int)w.start || pos > (int)(w.start + w.len)) continue;
      if (!spell_style(lexed_styles[w.start])) return false;
      if (checker()->check(lexed.substr(w.start, w.len))) return false;
      a = (int)w.start;
      b = (int)(w.start + w.len);
      return true;
    }
#endif
    (void)pos; (void)a; (void)b;
    return false;
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
    overlay_spelling();
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
    is_new = false;
    std::string first = data.substr(0, data.find('\n'));
    if (!lang_forced) lang = syntax::detect(path, first);
    apply_settings();
    update_tab_label(this);
    lsp_reopen();
    refresh_git();
    return true;
  }

  void refresh_git() {
    git_hunks = S.git_gutter && !path.empty() ? fld::git::diff_lines(path) : std::vector<fld::git::Hunk>{};
    if (ed) ed->redraw();
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
    is_new = false;
    update_tab_label(this);
    refresh_git();
    return true;
  }
};

