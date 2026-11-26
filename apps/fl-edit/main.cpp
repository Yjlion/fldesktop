// fl-edit: text editor for fldesktop
#include "edit_app.h"

Fl_Double_Window *g_win;
static Fl_Menu_Bar *g_menu;
fld::TabBar *g_tabbar;
static Fl_Group *g_area; // holds the documents; only the current one is shown
Fl_Box *g_status;

Settings S;



// ---- style table ---------------------------------------------------------------

// each style three times: plain, with an error underline and with a warning underline

Fl_Text_Display::Style_Table_Entry g_styles[NSTYLES];

Fl_Color rgb(unsigned v) { return fl_rgb_color((v >> 16) & 255, (v >> 8) & 255, v & 255); }
const syntax::Theme &cur_theme() { return syntax::theme(S.theme, fld::palette().dark); }

void make_styles() {
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

// ---- spell checking ------------------------------------------------------------------

#ifdef FLD_HAVE_SPELL
spell::Checker *checker() {
  static spell::Checker *c = new spell::Checker();
  return c;
}
#endif
bool spell_on() {
#ifdef FLD_HAVE_SPELL
  return S.spell && checker()->ok();
#else
  return false;
#endif
}

// ---- documents --------------------------------------------------------------------

std::vector<Doc *> g_docs;
Doc *g_current = nullptr;
Doc *current() { return g_current; }

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
  cfg.set_bool("Editor", "CheckSpelling", S.spell);
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
  S.spell = cfg.get_bool("Editor", "CheckSpelling", true);
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
void show_doc(Doc *d) {
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

void open_file(const std::string &p) {
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
    d->is_new = true;
    d->lang = syntax::detect(d->path, "");
    d->apply_settings();
    update_tab_label(d);
    update_status();
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

bool save(Doc *d) {
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
    case 28: format_document(); break;
    case 29: rename_symbol(); break;
    case 26: next_problem(false); break;
    case 27: next_problem(true); break;
    case 42: S.minimap = !S.minimap; apply_all(); break;
    case 45: S.spell = !S.spell; apply_all(); break;
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
void sync_menus() {
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
void editor_menu(Doc *d) {
  Fl_Text_Buffer *b = d->buf;
  bool sel = b->selected();
  auto run = [](long id) { return [id] { menu_cb(nullptr, (void *)id); }; };
  fld::Menu m;
#ifdef FLD_HAVE_SPELL
  int wa, wb;
  if (!sel && d->misspelt_at(d->ed->insert_position(), wa, wb)) {
    char *t = b->text_range(wa, wb);
    std::string word = t;
    free(t);
    std::vector<std::string> sug = checker()->suggest(word);
    for (auto &s : sug)
      m.item(fld::escape_at(s).c_str(), [d, wa, wb, s] {
        d->buf->replace(wa, wb, s.c_str());
        d->ed->insert_position(wa + (int)s.size());
      });
    if (sug.empty()) m.item("(no suggestions)", [] {}, FL_MENU_INACTIVE);
    m.divider();
    m.item(("Add \"" + fld::escape_at(word) + "\" to the Dictionary").c_str(), [word] {
      checker()->add_to_dictionary(word);
      for (Doc *x : g_docs) x->rehighlight();
    });
    m.item("Ignore", [word] {
      checker()->ignore(word);
      for (Doc *x : g_docs) x->rehighlight();
    }).divider();
  }
#endif
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
  g_menu = new fld::MenuBar(0, 0, W, MH);
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
  g_menu->add("&Code/Go to &Definition", FL_F + 12, menu_cb, (void *)25);
  g_menu->add("&Code/&Rename Symbol…", FL_F + 2, menu_cb, (void *)29);
  g_menu->add("&Code/&Format Document", FL_CTRL + FL_SHIFT + 'i', menu_cb, (void *)28, FL_MENU_DIVIDER);
  g_menu->add("&Code/&Next Problem", FL_F + 8, menu_cb, (void *)26);
  g_menu->add("&Code/&Previous Problem", FL_SHIFT + FL_F + 8, menu_cb, (void *)27, FL_MENU_DIVIDER);
  g_menu->add("&Code/&Language Servers", 0, menu_cb, (void *)43, FL_MENU_TOGGLE | (S.lsp ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Line Numbers", 0, menu_cb, (void *)30, FL_MENU_TOGGLE | (S.line_numbers ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Word Wrap", FL_ALT + 'z', menu_cb, (void *)31, FL_MENU_TOGGLE | (S.wrap ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Syntax Highlighting", 0, menu_cb, (void *)32, FL_MENU_TOGGLE | (S.highlight ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Highlight Matching Brackets", 0, menu_cb, (void *)41, FL_MENU_TOGGLE | (S.brackets ? FL_MENU_VALUE : 0));
  g_menu->add("&View/&Minimap", 0, menu_cb, (void *)42, FL_MENU_TOGGLE | (S.minimap ? FL_MENU_VALUE : 0));
#ifdef FLD_HAVE_SPELL
  g_menu->add("&View/Check S&pelling", FL_F + 7, menu_cb, (void *)45, FL_MENU_TOGGLE | (S.spell ? FL_MENU_VALUE : 0));
#endif
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
  fld::remember_window_size(g_win, "fl-edit");
  g_win->show(); // not show(argc, argv): it resets the theme colors
  current()->ed->take_focus();
  update_status();
  return Fl::run();
}
