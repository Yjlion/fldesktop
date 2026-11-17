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
#include <FL/Fl_Tabs.H>
#include <FL/Fl_Text_Buffer.H>
#include <FL/Fl_Text_Editor.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "syntax.h"

static Fl_Double_Window *g_win;
static Fl_Menu_Bar *g_menu;
static Fl_Tabs *g_tabs;
static Fl_Box *g_status;

struct Settings {
  bool line_numbers = true, wrap = false, highlight = true, spaces = false, auto_indent = true;
  int font_size = 14, tab_width = 4;
  std::vector<std::string> recent;
} S;

static const int MAX_HIGHLIGHT = 4 << 20; // skip highlighting huge files

// ---- style table ---------------------------------------------------------------

static Fl_Text_Display::Style_Table_Entry g_styles[9];

static void make_styles() {
  bool dark = fld::palette().dark;
  Fl_Font f = fld::mono_font();
  int s = S.font_size;
  auto rgb = [](unsigned v) { return fl_rgb_color((v >> 16) & 255, (v >> 8) & 255, v & 255); };
  Fl_Color plain = dark ? rgb(0xdcdfe4) : rgb(0x24262b);
  g_styles[0] = {plain, f, s, 0, 0};                                          // A plain
  g_styles[1] = {dark ? rgb(0x7f848e) : rgb(0x8a8f98), f + FL_ITALIC, s, 0, 0}; // B comment
  g_styles[2] = {dark ? rgb(0x98c379) : rgb(0x26a269), f, s, 0, 0};           // C string
  g_styles[3] = {dark ? rgb(0xc678dd) : rgb(0x8f3cb5), f + FL_BOLD, s, 0, 0}; // D keyword
  g_styles[4] = {dark ? rgb(0xd19a66) : rgb(0xc64600), f, s, 0, 0};           // E number
  g_styles[5] = {dark ? rgb(0xe5c07b) : rgb(0x9c6500), f, s, 0, 0};           // F preprocessor
  g_styles[6] = {dark ? rgb(0x56b6c2) : rgb(0x1a7f99), f, s, 0, 0};           // G type
  g_styles[7] = {dark ? rgb(0x61afef) : rgb(0x1c5aa8), f + FL_BOLD, s, 0, 0}; // H heading
  g_styles[8] = {dark ? rgb(0x61afef) : rgb(0x1c5aa8), f, s, 0, 0};           // I function
}

// ---- documents --------------------------------------------------------------------

class Doc;
static std::vector<Doc *> g_docs;
static void update_status();
static void update_tab_label(Doc *d);

class Editor : public Fl_Text_Editor {
public:
  Doc *doc;
  Editor(int x, int y, int w, int h, Doc *d) : Fl_Text_Editor(x, y, w, h), doc(d) {}
  int handle(int e) override;
};

class Doc : public Fl_Group {
public:
  std::string path;
  bool modified = false;
  bool loading = false;
  Fl_Text_Buffer *buf, *style;
  Editor *ed;
  syntax::Language lang = syntax::NONE;
  std::string tab_label;

  Doc(int x, int y, int w, int h) : Fl_Group(x, y, w, h) {
    buf = new Fl_Text_Buffer();
    style = new Fl_Text_Buffer();
    buf->tab_distance(S.tab_width);
    ed = new Editor(x, y, w, h, this);
    ed->buffer(buf);
    ed->textfont(fld::mono_font());
    ed->textsize(S.font_size);
    ed->box(FL_FLAT_BOX);
    ed->color(fld::palette().dark ? fl_rgb_color(0x1e, 0x1f, 0x22) : FL_WHITE);
    ed->cursor_color(fld::palette().accent);
    ed->cursor_style(Fl_Text_Display::SIMPLE_CURSOR);
    ed->selection_color(fld::palette().dark ? fl_rgb_color(0x3a, 0x4d, 0x6b) : fl_rgb_color(0xb4, 0xd2, 0xf5));
    ed->linenumber_font(fld::mono_font());
    ed->linenumber_size(S.font_size - 2);
    ed->linenumber_bgcolor(fld::palette().dark ? fl_rgb_color(0x26, 0x27, 0x2b) : fl_rgb_color(0xf3, 0xf4, 0xf6));
    ed->linenumber_fgcolor(fld::palette().fg_dim);
    ed->scrollbar_size(12);
    resizable(ed);
    end();
    when(FL_WHEN_CLOSED);
    buf->add_modify_callback(modify_cb, this);
    apply_settings();
  }
  ~Doc() override {
    buf->remove_modify_callback(modify_cb, this);
    ed->buffer(nullptr);
    ed->highlight_data(nullptr, nullptr, 0, 0, nullptr, nullptr);
    delete buf;
    delete style;
  }

  void apply_settings() {
    make_styles();
    ed->textsize(S.font_size);
    ed->linenumber_size(std::max(8, S.font_size - 2));
    ed->linenumber_width(S.line_numbers ? std::max(40, (int)(S.font_size * 3.5)) : 0);
    ed->wrap_mode(S.wrap ? Fl_Text_Display::WRAP_AT_BOUNDS : Fl_Text_Display::WRAP_NONE, 0);
    buf->tab_distance(S.tab_width);
    if (S.highlight && lang != syntax::NONE && buf->length() < MAX_HIGHLIGHT)
      ed->highlight_data(style, g_styles, 9, 'A', nullptr, nullptr);
    else
      ed->highlight_data(nullptr, nullptr, 0, 0, nullptr, nullptr);
    rehighlight();
    ed->redraw();
  }

  void rehighlight() {
    if (!S.highlight || lang == syntax::NONE || buf->length() >= MAX_HIGHLIGHT) return;
    char *text = buf->text();
    std::string t(text);
    free(text);
    std::string st;
    syntax::highlight(lang, t, st);
    style->text(st.c_str());
    ed->redisplay_range(0, buf->length());
  }

  static void highlight_later(void *d) { static_cast<Doc *>(d)->rehighlight(); }

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
    // keep the style buffer the same length right away, re-lex shortly after
    if (d->style->length() != d->buf->length() && S.highlight && d->lang != syntax::NONE) {
      if (del) d->style->remove(pos, std::min(pos + del, d->style->length()));
      if (ins) {
        std::string fill(ins, 'A');
        d->style->insert(pos, fill.c_str());
      }
    }
    Fl::remove_timeout(highlight_later, d);
    Fl::add_timeout(0.15, highlight_later, d);
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
    lang = syntax::detect(path, first);
    apply_settings();
    update_tab_label(this);
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
      lang = syntax::detect(path, s.substr(0, s.find('\n')));
      apply_settings();
    }
    modified = false;
    update_tab_label(this);
    return true;
  }
};

static Doc *current() { return g_tabs->value() ? (Doc *)g_tabs->value() : nullptr; }

static void update_tab_label(Doc *d) {
  d->tab_label = (d->modified ? "● " : "") + fld::escape_at(d->display_name());
  d->label(d->tab_label.c_str());
  d->copy_tooltip(d->path.empty() ? "Not saved yet" : d->path.c_str());
  g_tabs->redraw();
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
           d->buf->selected() ? "Selection" : "");
  g_status->copy_label(buf);
}

int Editor::handle(int e) {
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
  if (e == FL_KEYBOARD || e == FL_PUSH || e == FL_DRAG || e == FL_RELEASE) update_status();
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
  S.font_size = cfg.get_int("Editor", "FontSize", fld::mono_size());
  S.tab_width = std::max(1, std::min(16, cfg.get_int("Editor", "TabWidth", 4)));
  S.recent = fld::split(cfg.get("Editor", "Recent", ""), ';');
}

static void rebuild_recent_menu();

static void add_recent(const std::string &p) {
  S.recent.erase(std::remove(S.recent.begin(), S.recent.end(), p), S.recent.end());
  S.recent.insert(S.recent.begin(), p);
  if (S.recent.size() > 10) S.recent.resize(10);
  save_settings();
  rebuild_recent_menu();
}

// ---- document management ---------------------------------------------------------------

static void close_doc(Doc *d);

static Doc *new_doc() {
  int x = g_tabs->x(), y = g_tabs->y() + 30, w = g_tabs->w(), h = g_tabs->h() - 30;
  g_tabs->begin();
  Doc *d = new Doc(x, y, w, h);
  g_tabs->end();
  g_docs.push_back(d);
  // Fl_Tabs calls the child's callback when its close button is clicked
  d->callback([](Fl_Widget *w, void *) {
    if (Fl::callback_reason() == FL_REASON_CLOSED) Fl::add_timeout(0, [](void *p) { close_doc((Doc *)p); }, w);
  });
  update_tab_label(d);
  g_tabs->value(d);
  d->ed->take_focus();
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
    g_tabs->value(d);
    update_tab_label(d);
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
  g_tabs->value(d);
  update_tab_label(d);
  update_status();
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
  g_tabs->value(d);
  int c = fl_choice("Save changes to \"%s\" before closing?", "Cancel", "Save", "Don't Save",
                    d->display_name().c_str());
  if (c == 0) return false;
  if (c == 1) return save(d);
  return true;
}

static void close_doc(Doc *d) {
  if (!confirm_close(d)) return;
  g_docs.erase(std::remove(g_docs.begin(), g_docs.end(), d), g_docs.end());
  Fl::remove_timeout(Doc::highlight_later, d);
  g_tabs->remove(d);
  Fl::delete_widget(d);
  if (g_docs.empty()) new_doc();
  else g_tabs->value(g_docs.back());
  update_tab_label(current());
  update_status();
  g_tabs->redraw();
}

static void quit() {
  for (auto d : std::vector<Doc *>(g_docs))
    if (!confirm_close(d)) return;
  for (auto d : g_docs) d->modified = false;
  save_settings();
  g_win->hide();
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
    case 38: case 39: case 40: S.tab_width = id == 38 ? 2 : id == 39 ? 4 : 8; apply_all(); break;
    case 50: fld::about_dialog("Text Editor", "fl-edit", "A text and code editor built with FLTK."); break;
    case 60: if (d) { if (!d->path.empty()) fld::spawn({"fl-term", "--workdir", fld::dirname(d->path)}); else fld::spawn({"fl-term"}); } break;
  }
  if (d && id >= 10 && id < 20) update_status();
}

static void recent_cb(Fl_Widget *, void *v) { open_file((const char *)v); }

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

static void tabs_cb(Fl_Widget *, void *) {
  Doc *d = current();
  if (d) {
    update_tab_label(d);
    d->ed->take_focus();
  }
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
  g_menu->add("&Edit/&Go to Line…", FL_CTRL + 'g', menu_cb, (void *)20);
  g_menu->add("&View/&Line Numbers", 0, menu_cb, (void *)30, FL_MENU_TOGGLE | (S.line_numbers ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Word Wrap", FL_ALT + 'z', menu_cb, (void *)31, FL_MENU_TOGGLE | (S.wrap ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Syntax Highlighting", 0, menu_cb, (void *)32, FL_MENU_TOGGLE | (S.highlight ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  g_menu->add("&View/Zoom &In", FL_CTRL + '=', menu_cb, (void *)33);
  g_menu->add("&View/Zoom &Out", FL_CTRL + '-', menu_cb, (void *)34);
  g_menu->add("&View/&Normal Size", FL_CTRL + '0', menu_cb, (void *)35);
  g_menu->add("&Options/Insert &Spaces for Tab", 0, menu_cb, (void *)36, FL_MENU_TOGGLE | (S.spaces ? FL_MENU_VALUE : 0));
  g_menu->add("&Options/&Auto Indent", 0, menu_cb, (void *)37, FL_MENU_TOGGLE | (S.auto_indent ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER);
  g_menu->add("&Options/Tab Width/2", 0, menu_cb, (void *)38, FL_MENU_RADIO | (S.tab_width == 2 ? FL_MENU_VALUE : 0));
  g_menu->add("&Options/Tab Width/4", 0, menu_cb, (void *)39, FL_MENU_RADIO | (S.tab_width == 4 ? FL_MENU_VALUE : 0));
  g_menu->add("&Options/Tab Width/8", 0, menu_cb, (void *)40, FL_MENU_RADIO | (S.tab_width == 8 ? FL_MENU_VALUE : 0));
  g_menu->add("&Help/&About", 0, menu_cb, (void *)50);
  rebuild_recent_menu();

  g_tabs = new Fl_Tabs(0, MH, W, H - MH - SH);
  g_tabs->handle_overflow(Fl_Tabs::OVERFLOW_PULLDOWN);
  g_tabs->callback(tabs_cb);
  g_tabs->when(FL_WHEN_CHANGED);
  g_tabs->end();
  g_status = new Fl_Box(0, H - SH, W, SH);
  g_status->box(FL_FLAT_BOX);
  g_status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
  g_status->labelsize(12);
  g_status->labelcolor(fld::palette().fg_dim);
  g_win->resizable(g_tabs);
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
    if (a[0] == '-') continue;
    if (fld::starts_with(a, "file://")) a = fld::uri_decode(a.substr(7));
    if (opened == 0 && g_docs.empty()) new_doc();
    open_file(a);
    opened++;
  }
  if (g_docs.empty()) new_doc();
  g_win->show(); // not show(argc, argv): it resets the theme colors
  current()->ed->take_focus();
  update_status();
  return Fl::run();
}
