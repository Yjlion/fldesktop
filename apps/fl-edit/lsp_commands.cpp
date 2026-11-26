// fl-edit: language server commands (completion, definition, format, rename, problems)
#include "edit_app.h"

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
void complete() {
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
void go_to_definition() {
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

// edits from the language server to a document's text, as one undo step
static bool apply_lsp_edits(Doc *d, const std::vector<lsp::TextEdit> &edits) {
  if (edits.empty()) return true;
  char *t = d->buf->text();
  std::string text = t;
  free(t);
  std::string before = text;
  if (!lsp::apply_edits(text, edits)) return false;
  if (text == before) return true;
  int pos = d->ed->insert_position();
  d->buf->replace(0, d->buf->length(), text.c_str());
  d->ed->insert_position(std::min(pos, d->buf->length()));
  d->ed->show_insert_position();
  return true;
}

// Ctrl+Shift+I
void format_document() {
  Doc *d = current();
  if (!need_server(d)) return;
  d->lsp_flush();
  lspc::format(d->lsp, d->path, S.tab_width, S.spaces, [d](const std::vector<lsp::TextEdit> &edits) {
    if (doc_index(d) < 0) return;
    if (edits.empty()) status_note("Nothing to change (or the server cannot format this file)");
    else if (!apply_lsp_edits(d, edits)) status_note("The server's changes did not fit the text");
    else status_note("Formatted");
  });
}

// F2
void rename_symbol() {
  Doc *d = current();
  if (!need_server(d)) return;
  d->lsp_flush();
  int pos = d->ed->insert_position(), line, col;
  int a = d->buf->word_start(pos), b = d->buf->word_end(pos);
  char *w = d->buf->text_range(a, b);
  std::string word = w;
  free(w);
  const char *nn = fl_input("Rename %s to:", word.c_str(), word.empty() ? "the symbol" : word.c_str());
  if (!nn || !*nn || word == nn) return;
  std::string new_name = nn;
  d->lsp_position(pos, line, col);
  lspc::rename(d->lsp, d->path, line, col, new_name,
               [](const std::vector<std::pair<std::string, std::vector<lsp::TextEdit>>> &files, const std::string &err) {
    if (!err.empty()) {
      status_note(err);
      return;
    }
    int changed = 0;
    for (auto &f : files) {
      // every file is opened (or shown) so the change can be looked at and saved
      open_file(f.first);
      Doc *t = current();
      if (t && t->path == fld::abs_path(f.first) && apply_lsp_edits(t, f.second)) changed++;
    }
    status_note("Renamed in " + std::to_string(changed) + (changed == 1 ? " file" : " files") + " (not saved yet)");
  });
}

// F8 and Shift+F8
void next_problem(bool back) {
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
