// fl-edit: the editor widget, carets, the minimap and the status bar
#include "edit_app.h"

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

int doc_index(Doc *d) {
  auto it = std::find(g_docs.begin(), g_docs.end(), d);
  return it == g_docs.end() ? -1 : (int)(it - g_docs.begin());
}

void refresh_tabbar() {
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


void update_tab_label(Doc *d) {
  if (!d) return;
  refresh_tabbar();
  if (d == current()) {
    std::string title = (d->modified ? "*" : "") + d->display_name() + " – Text Editor";
    g_win->copy_label(title.c_str());
  }
}

void update_status() {
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
  if (d->is_new) st += "    New file";
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
