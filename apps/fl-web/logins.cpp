// fl-web: reader mode and saved logins
#include "web_app.h"

// ---- reader mode ------------------------------------------------------------

// F9: the page's article alone, in a plain layout; F9 again goes back to the page
void Browser::toggle_reader() {
#ifdef FLD_HAVE_READER
  Tab *t = tab();
  if (!t) return;
  if (t->reader) {
    t->reader = false;
    webkit_web_view_load_uri(t->wv->view(), t->reader_of.c_str());
    glib_loop_poke();
    return;
  }
  if (t->uri.empty() || t->loading) return;
  struct Job {
    Browser *b;
    Tab *t;
    std::string uri;
  };
  webkit_web_view_evaluate_javascript(
      t->wv->view(), "document.documentElement.outerHTML", -1, nullptr, nullptr, nullptr,
      [](GObject *o, GAsyncResult *r, gpointer d) {
        std::unique_ptr<Job> job((Job *)d);
        GError *e = nullptr;
        JSCValue *v = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(o), r, &e);
        std::string html;
        if (v) {
          char *s = jsc_value_to_string(v);
          html = s ? s : "";
          g_free(s);
          g_object_unref(v);
        }
        if (e) g_error_free(e);
        Job j = *job;
        later([j, html] {
          if (!tab_alive(j.t) || j.t->uri != j.uri) return;
          reader::Article a = reader::extract(html, j.uri);
          if (!a.ok()) {
            j.b->set_status("Reader mode: no article found on this page", 3);
            return;
          }
          if (a.title.empty()) a.title = fallback_title(j.uri);
          j.t->reader = true;
          j.t->reader_of = j.uri;
          std::string page = reader::page(a, fld::palette().dark, g_cfg.reader_font);
          webkit_web_view_load_html(j.t->wv->view(), page.c_str(), j.uri.c_str());
          glib_loop_poke();
        });
      },
      new Job{this, t, t->uri});
  glib_loop_poke();
#endif
}

// ---- saved logins -----------------------------------------------------------

#ifdef FLD_HAVE_SECRET
static bool g_secret_ok = false, g_secret_tried = false;
static bool secret_ready() {
  if (!g_secret_tried) {
    g_secret_tried = true;
    std::string err;
    g_secret_ok = secret::connect(err);
  }
  return g_secret_ok;
}

static std::string item_attr(const secret::Item &it, const char *key) {
  for (auto &a : it.attributes)
    if (a.first == key) return a.second;
  return "";
}

static std::vector<secret::Item> saved_logins(const std::string &origin) {
  if (!secret_ready()) return {};
  std::vector<std::pair<std::string, std::string>> q{{"xdg:schema", kLoginSchema}};
  if (!origin.empty()) q.push_back({"origin", origin});
  auto items = secret::search(q);
  std::sort(items.begin(), items.end(), [](const secret::Item &a, const secret::Item &b) { return a.modified > b.modified; });
  return items;
}

// a login a page sent: offer to keep it (or the new password of a saved one)
void login_submitted(const std::string &json) {
  std::string origin, user, pass;
  if (!web::parse_login_message(json, origin, user, pass) || !g_cfg.save_passwords) return;
  if (std::find(g_cfg.logins_never.begin(), g_cfg.logins_never.end(), origin) != g_cfg.logins_never.end()) return;
  // the window showing that site (private windows keep nothing)
  Browser *b = nullptr;
  for (Browser *w : g_windows)
    if (w->tab() && login_origin(w->tab()->uri) == origin) b = w;
  if (!b || b->priv || !secret_ready()) return;
  std::string have;
  std::string existing;
  for (auto &it : saved_logins(origin))
    if (item_attr(it, "username") == user) existing = it.path;
  if (!existing.empty()) {
    std::string err;
    if (!secret::get_secret(existing, have, err) && !secret::unlock(existing, err)) return;
    if (have.empty()) secret::get_secret(existing, have, err);
    if (have == pass) return; // nothing new
  }
  later([b, origin, user, pass, existing] {
    std::string who = user.empty() ? origin : user + " on " + origin;
    int c = fl_choice(existing.empty() ? "Save the password for %s in the keyring?" : "Update the saved password for %s?",
                      "Not Now", existing.empty() ? "Save" : "Update", existing.empty() ? "Never for This Site" : nullptr,
                      fld::escape_at(who).c_str());
    std::string err;
    if (c == 1) {
      bool ok;
      if (!existing.empty()) ok = secret::set_secret(existing, pass, err);
      else {
        std::string coll = secret::default_collection();
        if (!coll.empty() && !secret::unlock(coll, err)) coll.clear();
        ok = !coll.empty() &&
             secret::create_item(coll, "Web login for " + who,
                                 {{"xdg:schema", kLoginSchema}, {"origin", origin}, {"username", user}}, pass, err);
      }
      if (!ok) fl_alert("The password could not be saved: %s", fld::escape_at(err.empty() ? "no keyring" : err).c_str());
      else b->set_status("Password saved", 2);
    } else if (c == 2) {
      g_cfg.logins_never.push_back(origin);
      g_cfg.save();
    }
  });
}
#endif

// After a page has loaded (choose=false): fills its login form with the
// newest saved login when the keyring is open. From the menu (choose=true):
// asks to open the keyring and which login to use when there are several.
void Browser::fill_login(bool choose) {
#ifdef FLD_HAVE_SECRET
  Tab *t = tab();
  if (!t || t->reader || (t->filled && !choose)) return;
  std::string origin = login_origin(t->uri);
  if (origin.empty() || !secret_ready()) return;
  auto items = saved_logins(origin);
  if (items.empty()) {
    if (choose) set_status("No saved login for " + origin, 3);
    return;
  }
  size_t pick = 0;
  if (choose && items.size() > 1) {
    std::vector<std::string> labels;
    for (auto &it : items) labels.push_back(fld::Menu::text(item_attr(it, "username").empty() ? "(no user name)" : item_attr(it, "username")));
    std::vector<Fl_Menu_Item> menu;
    for (size_t i = 0; i < labels.size(); i++) menu.push_back({labels[i].c_str(), 0, nullptr, (void *)i});
    menu.push_back({nullptr});
    const Fl_Menu_Item *r = menu[0].popup(content->x() + 40, content->y() + 20);
    if (!r) return;
    pick = (size_t)(long)r->user_data();
  }
  const secret::Item &it = items[pick];
  std::string err, pass;
  if (it.locked) {
    if (!choose || !secret::unlock(it.path, err)) return;
  }
  if (!secret::get_secret(it.path, pass, err) || pass.empty()) return;
  std::string js = login_fill_script(item_attr(it, "username"), pass);
  t->filled = true;
  webkit_web_view_evaluate_javascript(t->wv->view(), js.c_str(), -1, nullptr, nullptr, nullptr, nullptr, nullptr);
  glib_loop_poke();
#else
  (void)choose;
#endif
}

// Saved Passwords: the logins in the keyring, to look at, copy and remove
void show_passwords() {
#ifdef FLD_HAVE_SECRET
  if (!secret_ready()) {
    fl_alert("No password store (Secret Service) is running. Install GNOME Keyring or KeePassXC.");
    return;
  }
  struct Dlg {
    Fl_Double_Window *w;
    Fl_Hold_Browser *list;
    Fl_Input *filter;
    Fl_Box *shown;
    std::vector<secret::Item> items, view;
  } d;
  d.w = new Fl_Double_Window(560, 420, "Saved Passwords");
  d.filter = new Fl_Input(12, 12, 536, 28);
  d.filter->tooltip("Search sites and user names");
  d.filter->when(FL_WHEN_CHANGED);
  d.list = new Fl_Hold_Browser(12, 48, 536, 260);
  static int cols[] = {300, 0};
  d.list->column_widths(cols);
  d.list->column_char('\t');
  d.list->format_char(0);
  d.shown = new Fl_Box(12, 314, 536, 26);
  d.shown->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_CLIP);
  auto *show = new Fl_Button(12, 348, 90, 30, "Show");
  auto *copy = new Fl_Button(108, 348, 90, 30, "Copy");
  auto *open = new Fl_Button(204, 348, 90, 30, "Open Site");
  auto *del = new Fl_Button(300, 348, 90, 30, "Remove");
  auto *imp = new Fl_Button(396, 348, 152, 30, "Import from CSV…");
  imp->tooltip("Passwords exported from Firefox or Chrome (a .csv file)");
  auto *close = new Fl_Return_Button(448, 380, 100, 30, "Close");
  close->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
  auto refill = [](Fl_Widget *, void *v) {
    Dlg &d = *(Dlg *)v;
    std::string f = fld::lower(d.filter->value());
    d.list->clear();
    d.view.clear();
    for (auto &it : d.items) {
      std::string site = item_attr(it, "origin"), user = item_attr(it, "username");
      if (!f.empty() && fld::lower(site + " " + user).find(f) == std::string::npos) continue;
      d.view.push_back(it);
      d.list->add((site + "\t" + (user.empty() ? "(no user name)" : user) + (it.locked ? "  (locked)" : "")).c_str());
    }
    d.shown->label(d.items.empty() ? "No saved passwords" : "");
  };
  d.items = saved_logins("");
  d.filter->callback(refill, &d);
  refill(nullptr, &d);
  auto selected = [](Dlg &d) -> const secret::Item * {
    int i = d.list->value() - 1;
    return i >= 0 && i < (int)d.view.size() ? &d.view[(size_t)i] : nullptr;
  };
  struct Ctx {
    Dlg *d;
    const secret::Item *(*sel)(Dlg &);
  };
  static Ctx ctx;
  ctx = {&d, +selected};
  auto get = [](Dlg &d, const secret::Item &it, std::string &pass) {
    std::string err;
    if (it.locked && !secret::unlock(it.path, err)) return false;
    if (!secret::get_secret(it.path, pass, err)) {
      fl_alert("%s", fld::escape_at(err).c_str());
      return false;
    }
    (void)d;
    return true;
  };
  static bool (*getp)(Dlg &, const secret::Item &, std::string &) = +get;
  show->callback([](Fl_Widget *, void *) {
    const secret::Item *it = ctx.sel(*ctx.d);
    std::string pass;
    if (it && getp(*ctx.d, *it, pass)) ctx.d->shown->copy_label(fld::escape_at("Password: " + pass).c_str());
  });
  copy->callback([](Fl_Widget *, void *) {
    const secret::Item *it = ctx.sel(*ctx.d);
    std::string pass;
    if (it && getp(*ctx.d, *it, pass)) {
      Fl::copy(pass.c_str(), (int)pass.size(), 1);
      ctx.d->shown->label("Copied to the clipboard");
    }
  });
  open->callback([](Fl_Widget *, void *) {
    const secret::Item *it = ctx.sel(*ctx.d);
    if (it && !g_windows.empty()) g_windows.front()->new_tab(item_attr(*it, "origin"), true);
  });
  del->callback([](Fl_Widget *, void *v) {
    Dlg &d = *(Dlg *)v;
    const secret::Item *it = ctx.sel(d);
    if (!it) return;
    std::string what = item_attr(*it, "username") + " on " + item_attr(*it, "origin");
    if (fl_choice("Remove the saved password for %s?", "Cancel", "Remove", nullptr, fld::escape_at(what).c_str()) != 1)
      return;
    std::string err;
    if (!secret::delete_item(it->path, err)) fl_alert("%s", fld::escape_at(err).c_str());
    d.items = saved_logins("");
    d.filter->do_callback();
  }, &d);
  imp->callback([](Fl_Widget *, void *v) {
    Dlg &d = *(Dlg *)v;
    Fl_Native_File_Chooser fc;
    fc.title("Import Passwords");
    fc.filter("Password exports\t*.csv");
    if (fc.show() != 0 || !fc.filename()) return;
    std::string text, err;
    if (!fld::read_file(fc.filename(), text, 32u << 20)) {
      fl_alert("Cannot read %s", fld::escape_at(fc.filename()).c_str());
      return;
    }
    auto logins = web::parse_password_csv(text, &err);
    if (logins.empty()) {
      fl_alert("%s", fld::escape_at(err.empty() ? "No passwords in this file." : err).c_str());
      return;
    }
    std::string coll = secret::default_collection();
    if (coll.empty() || !secret::unlock(coll, err)) {
      fl_alert("The keyring could not be opened: %s", fld::escape_at(err).c_str());
      return;
    }
    int added = 0, same = 0;
    for (auto &l : logins) {
      // not twice: the same site and user name is left as it is
      bool have = false;
      for (auto &it : d.items)
        if (item_attr(it, "origin") == l.origin && item_attr(it, "username") == l.user) have = true;
      if (have) { same++; continue; }
      std::string who = l.user.empty() ? l.origin : l.user + " on " + l.origin;
      if (secret::create_item(coll, "Web login for " + who,
                              {{"xdg:schema", kLoginSchema}, {"origin", l.origin}, {"username", l.user}}, l.pass, err))
        added++;
    }
    d.items = saved_logins("");
    d.filter->do_callback();
    d.shown->copy_label(("Imported " + std::to_string(added) + (same ? ", " + std::to_string(same) + " already saved" : "") +
                         ". Delete the CSV file: it holds the passwords unencrypted.").c_str());
  }, &d);
  d.list->callback([](Fl_Widget *, void *v) { ((Dlg *)v)->shown->label(""); }, &d);
  d.w->end();
  d.w->resizable(d.list);
  d.w->set_modal();
  d.w->show();
  while (d.w->shown()) Fl::wait();
  Fl::delete_widget(d.w);
#endif
}

// F12: WebKit's inspector frontend, served on loopback, in a window of its own
void Browser::open_devtools() {
  if (!g_inspector_port) {
    // WebKit starts its inspector server only at startup
    if (fl_choice("The developer tools are off. Turn them on and restart Web?", "Cancel", "Restart", nullptr) != 1)
      return;
    g_cfg.devtools = true;
    g_cfg.save();
    save_session();
    // the same tabs again, in a new process with the inspector server
    std::vector<std::string> args = {"fl-web"};
    for (auto &u : session_uris()) args.push_back(u);
    std::vector<char *> argv;
    for (auto &a : args) argv.push_back((char *)a.c_str());
    argv.push_back(nullptr);
    fld::restart_self((int)args.size(), argv.data());
    fl_alert("Web could not restart itself. Close it and open it again.");
    return;
  }
  Browser *b = new_window({"http://127.0.0.1:" + std::to_string(g_inspector_port) + "/"}, false);
  b->win->copy_label("Developer Tools — Web");
}
