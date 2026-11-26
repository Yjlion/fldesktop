// fl-web: the history, bookmarks and settings dialogs
#include "web_app.h"

// ---------------------------------------------------------------------------
// History, bookmarks and settings dialogs

static void list_dialog(const char *title, Browser *b, bool history) {
  struct Ctx {
    Browser *b;
    bool history;
    Fl_Input *filter;
    Fl_Hold_Browser *list;
    std::vector<std::string> urls;
    void fill() {
      list->clear();
      urls.clear();
      if (history) {
        for (auto &e : history_view(g_history, filter->value(), 1000)) {
          char when[32];
          strftime(when, sizeof when, "%Y-%m-%d %H:%M", localtime(&e.time));
          list->add((fld::escape_at(e.title.empty() ? fallback_title(e.url) : e.title) + "\t" + fld::escape_at(e.url) + "\t" + when).c_str());
          urls.push_back(e.url);
        }
      } else {
        std::string f = fld::lower(filter->value());
        for (auto &bm : g_bookmarks) {
          if (!f.empty() && fld::lower(bm.url + " " + bm.title).find(f) == std::string::npos) continue;
          list->add((fld::escape_at(bm.title.empty() ? fallback_title(bm.url) : bm.title) + "\t" + fld::escape_at(bm.url)).c_str());
          urls.push_back(bm.url);
        }
      }
    }
    std::string selected() const {
      int l = list->value();
      return l > 0 && l <= (int)urls.size() ? urls[(size_t)l - 1] : "";
    }
  };
  Fl_Double_Window *w = new Fl_Double_Window(720, 460, title);
  Ctx *c = new Ctx{b, history, nullptr, nullptr, {}};
  c->filter = new Fl_Input(10, 10, 700, 28);
  c->filter->tooltip("Search");
  c->filter->when(FL_WHEN_CHANGED);
  c->filter->callback([](Fl_Widget *, void *d) { ((Ctx *)d)->fill(); }, c);
  c->list = new Fl_Hold_Browser(10, 46, 700, 364);
  static int hcols[] = {300, 300, 0}, bcols[] = {300, 0};
  c->list->column_widths(history ? hcols : bcols);
  c->list->column_char('\t');
  c->list->callback([](Fl_Widget *, void *d) {
    Ctx *c = (Ctx *)d;
    if (Fl::event_clicks() && !c->selected().empty() && std::find(g_windows.begin(), g_windows.end(), c->b) != g_windows.end())
      c->b->load(c->selected());
  }, c);
  Fl_Button *open = new Fl_Button(10, 420, 100, 30, "Open");
  open->callback([](Fl_Widget *, void *d) {
    Ctx *c = (Ctx *)d;
    if (!c->selected().empty() && std::find(g_windows.begin(), g_windows.end(), c->b) != g_windows.end()) c->b->load(c->selected());
  }, c);
  Fl_Button *tab = new Fl_Button(116, 420, 130, 30, "Open in New Tab");
  tab->callback([](Fl_Widget *, void *d) {
    Ctx *c = (Ctx *)d;
    if (!c->selected().empty() && std::find(g_windows.begin(), g_windows.end(), c->b) != g_windows.end())
      c->b->new_tab(c->selected(), true);
  }, c);
  Fl_Button *del = new Fl_Button(252, 420, 100, 30, "Remove");
  del->callback([](Fl_Widget *, void *d) {
    Ctx *c = (Ctx *)d;
    std::string u = c->selected();
    if (u.empty()) return;
    if (c->history) {
      g_history.erase(std::remove_if(g_history.begin(), g_history.end(), [&](const HistoryEntry &e) { return e.url == u; }), g_history.end());
      std::string data;
      for (auto &e : g_history) data += std::to_string((long long)e.time) + "\t" + one_line(e.url) + "\t" + one_line(e.title) + "\n";
      fld::write_file(history_path(), data);
    } else {
      g_bookmarks.erase(std::remove_if(g_bookmarks.begin(), g_bookmarks.end(), [&](const Bookmark &b) { return b.url == u; }), g_bookmarks.end());
      save_bookmarks(bookmarks_path(), g_bookmarks);
      for (auto *w : g_windows) w->update_chrome();
    }
    c->fill();
  }, c);
  if (history) {
    Fl_Button *clear = new Fl_Button(610, 420, 100, 30, "Clear All");
    clear->callback([](Fl_Widget *, void *d) {
      if (fl_choice("Clear the whole browsing history?", "Cancel", "Clear", nullptr) != 1) return;
      g_history.clear();
      fld::write_file(history_path(), "");
      ((Ctx *)d)->fill();
    }, c);
  } else {
    Fl_Button *rename = new Fl_Button(358, 420, 100, 30, "Rename…");
    rename->callback([](Fl_Widget *, void *d) {
      Ctx *c = (Ctx *)d;
      std::string u = c->selected();
      for (auto &bm : g_bookmarks)
        if (bm.url == u) {
          const char *n = fl_input("Name:", bm.title.c_str());
          if (n) { bm.title = n; save_bookmarks(bookmarks_path(), g_bookmarks); }
        }
      c->fill();
    }, c);
  }
  w->resizable(c->list);
  w->end();
  w->callback([](Fl_Widget *w, void *d) {
    w->hide();
    Fl::delete_widget(w);
    delete (Ctx *)d;
  }, c);
  c->fill();
  w->show();
}

void show_history(Browser *b) { list_dialog("History", b, true); }
void show_bookmarks(Browser *b) { list_dialog("Bookmarks", b, false); }

void apply_settings() {
  webkit_settings_set_enable_javascript(g_settings, g_cfg.javascript);
  WebView::natural_scroll = g_cfg.natural_scroll;
}

void show_settings() {
  Fl_Double_Window dlg(480, 600, "Web Settings");
  Fl_Input home(140, 14, 326, 28, "Home page:");
  home.value(g_cfg.home.c_str());
  Fl_Choice search(140, 50, 200, 28, "Search with:");
  int sel = 0, i = 0;
  for (auto &e : search_engines()) {
    search.add(e.name);
    if (fld::lower(g_cfg.search) == fld::lower(e.name)) sel = i;
    i++;
  }
  search.value(sel);
  Fl_Check_Button js(140, 86, 300, 26, "Run JavaScript");
  js.value(g_cfg.javascript);
  Fl_Check_Button restore(140, 114, 300, 26, "Reopen the tabs of the last session");
  restore.value(g_cfg.restore);
  Fl_Check_Button natural(140, 142, 330, 26, "Natural scrolling (the page follows the fingers)");
  natural.value(g_cfg.natural_scroll);
  Fl_Check_Button menubar(140, 170, 330, 26, "Show the menu bar");
  menubar.value(g_cfg.menubar);
  Fl_Check_Button devtools(140, 198, 330, 26, "Developer tools (F12; after a restart)");
  devtools.value(g_cfg.devtools);
  Fl_Box adlabel(14, 236, 120, 26, "Ad blocking:");
  adlabel.align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  Fl_Check_Button ads(140, 236, 330, 26, "Block ads and trackers");
  ads.value(g_cfg.block_ads);
  Fl_Check_Button easylist(160, 262, 140, 26, "EasyList (ads)");
  easylist.value(g_cfg.easylist);
  Fl_Check_Button easyprivacy(300, 262, 170, 26, "EasyPrivacy (trackers)");
  easyprivacy.value(g_cfg.easyprivacy);
  Fl_Input extra(160, 292, 306, 28);
  extra.tooltip("Addresses of more filter lists in Adblock Plus format, separated by spaces");
  {
    std::string e;
    for (auto &u : g_cfg.adblock_extra) e += (e.empty() ? "" : " ") + u;
    extra.value(e.c_str());
  }
  Fl_Box extra_label(14, 292, 144, 28, "More lists:");
  extra_label.align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  Fl_Box adstatus(160, 324, 306, 22);
  adstatus.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
  adstatus.labelsize(fld::ui_font_size() - 1);
  adstatus.copy_label(web::adblock::status().c_str());
  web::adblock::on_status = [&adstatus] { adstatus.copy_label(web::adblock::status().c_str()); adstatus.redraw(); };
  Fl_Button update(160, 350, 160, 28, "Update Lists Now");
  update.callback([](Fl_Widget *, void *) { web::adblock::update_now(); });
  auto sync = [](Fl_Widget *, void *d) {
    Fl_Check_Button **b = (Fl_Check_Button **)d;
    if (b[0]->value()) { b[1]->activate(); b[2]->activate(); }
    else { b[1]->deactivate(); b[2]->deactivate(); }
  };
  Fl_Check_Button *ad_buttons[3] = {&ads, &easylist, &easyprivacy};
  ads.callback(sync, ad_buttons);
  sync(&ads, ad_buttons);
  int py = 390;
  Fl_Box privacy(14, py, 120, 28, "Privacy:");
  privacy.align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  Fl_Button clear(140, py, 230, 28, "Clear Cookies and Site Data…");
  clear.callback([](Fl_Widget *, void *) {
    if (fl_choice("Remove all cookies, caches and data that web sites stored? You will be signed out of sites.",
                  "Cancel", "Clear", nullptr) != 1)
      return;
    webkit_website_data_manager_clear(webkit_network_session_get_website_data_manager(g_session),
                                      WEBKIT_WEBSITE_DATA_ALL, 0, nullptr, nullptr, nullptr);
    glib_loop_poke();
  });
  Fl_Button defb(140, py + 36, 230, 28, "Make Web the Default Browser");
  defb.callback([](Fl_Widget *, void *) {
    bool ok = fld::spawn({"xdg-settings", "set", "default-web-browser", "fl-web.desktop"});
    fl_message(ok ? "Web is now the default browser." : "Could not run xdg-settings.");
  });
  Fl_Box pwlabel(14, py + 76, 120, 26, "Passwords:");
  pwlabel.align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  Fl_Check_Button savepw(140, py + 76, 330, 26, "Offer to save passwords in the keyring");
  savepw.value(g_cfg.save_passwords);
  Fl_Button pwlist(140, py + 106, 230, 28, "Saved Passwords…");
  pwlist.callback([](Fl_Widget *, void *) { show_passwords(); });
#ifndef FLD_HAVE_SECRET
  savepw.deactivate();
  pwlist.deactivate();
#endif
  Fl_Button cancel(268, 558, 96, 30, "Cancel");
  Fl_Return_Button ok(370, 558, 96, 30, "Save");
  int result = -1;
  cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 0; }, &result);
  ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
  dlg.end();
  dlg.set_modal();
  dlg.show();
  while (result < 0 && dlg.shown()) Fl::wait();
  web::adblock::on_status = nullptr;
  if (result != 1) return;
  std::string h = fld::trim(home.value());
  g_cfg.home = h.empty() ? "about:blank" : url_from_input(h, search_template(g_cfg.search));
  g_cfg.search = search_engines()[(size_t)std::max(0, search.value())].name;
  g_cfg.javascript = js.value();
  g_cfg.restore = restore.value();
  g_cfg.natural_scroll = natural.value();
  g_cfg.menubar = menubar.value();
  g_cfg.devtools = devtools.value();
  g_cfg.block_ads = ads.value();
  g_cfg.save_passwords = savepw.value();
  g_cfg.easylist = easylist.value();
  g_cfg.easyprivacy = easyprivacy.value();
  g_cfg.adblock_extra.clear();
  for (auto &u : fld::split(extra.value(), ' '))
    if (fld::trim(u).compare(0, 8, "https://") == 0 || fld::trim(u).compare(0, 7, "http://") == 0)
      g_cfg.adblock_extra.push_back(fld::trim(u));
  g_cfg.save();
  apply_settings();
  web::adblock::apply(g_cfg.adblock());
  for (auto *w : g_windows) w->layout();
}
