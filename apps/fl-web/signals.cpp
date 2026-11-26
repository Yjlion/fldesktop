// fl-web: WebKit signal handlers, the commands, the context and tab menus and the menu bar
#include "web_app.h"

int BrowserWindow::handle(int e) {
  if (e == FL_SHORTCUT && b->shortcut(Fl::event_key(), Fl::event_state())) return 1;
  int r = Fl_Double_Window::handle(e);
  if (e == FL_KEYBOARD && !r && b->shortcut(Fl::event_key(), Fl::event_state())) return 1;
  return r;
}


void BrowserWindow::hide() {
  if (std::count_if(g_windows.begin(), g_windows.end(), [](Browser *w) { return !w->priv; }) == 1 && !b->priv)
    save_session();
  Fl_Double_Window::hide();
  // the window goes once its tabs are closed (later: we may be inside one of their callbacks)
  Fl::add_timeout(0, [](void *d) {
    Browser *b = (Browser *)d;
    while (!b->tabs.empty()) {
      Tab *t = b->tabs.back();
      g_signal_handlers_disconnect_by_data(t->wv->view(), t);
      b->tabs.pop_back();
      b->content->remove(t->wv);
      delete t->wv;
      delete t;
    }
    g_windows.erase(std::remove(g_windows.begin(), g_windows.end(), b), g_windows.end());
    Fl::remove_timeout(Browser::hide_status, b);
    b->remove_status();
    Fl::delete_widget(b->win);
    delete b;
  }, b);
}

// ---------------------------------------------------------------------------
// WebKit signal handlers

std::string str(const char *s) { return s ? s : ""; }

static double now_s() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec / 1e9;
}

// a signal handler from a lambda without captures (G_CALLBACK is a macro:
// commas in the lambda would split its argument)

// runs fn outside of GLib's dispatch (dialogs run nested FLTK loops)
void later(std::function<void()> fn) {
  auto *f = new std::function<void()>(std::move(fn));
  Fl::add_timeout(0, [](void *d) {
    auto *fp = (std::function<void()> *)d;
    (*fp)();
    delete fp;
    glib_loop_poke();
  }, f);
}

bool tab_alive(Tab *t) {
  for (auto *w : g_windows)
    if (std::find(w->tabs.begin(), w->tabs.end(), t) != w->tabs.end()) return true;
  return false;
}

void Browser::connect(Tab *t) {
  WebKitWebView *v = t->wv->view();
  auto chrome = +[](GObject *, GParamSpec *, gpointer d) {
    Tab *t = (Tab *)d;
    WebKitWebView *v = t->wv->view();
    t->title = str(webkit_web_view_get_title(v));
    t->uri = str(webkit_web_view_get_uri(v));
    t->progress = webkit_web_view_get_estimated_load_progress(v);
    t->loading = webkit_web_view_is_loading(v);
    if (t->win->tab() == t) t->win->update_chrome();
    else t->win->refresh_tabs();
  };
  for (const char *p : {"notify::title", "notify::uri", "notify::estimated-load-progress", "notify::is-loading"})
    g_signal_connect(v, p, G_CALLBACK(chrome), t);

  g_signal_connect(v, "load-changed", CB([](WebKitWebView *v, WebKitLoadEvent ev, gpointer d) {
    Tab *t = (Tab *)d;
    if (ev == WEBKIT_LOAD_COMMITTED) {
      // leaving the article (a link, back, another address) ends reader mode
      if (t->reader && str(webkit_web_view_get_uri(v)) != t->reader_of) t->reader = false;
      t->filled = false;
      // the zoom chosen for this site before
      double z = site_zoom(str(webkit_web_view_get_uri(v)));
      if (std::fabs(webkit_web_view_get_zoom_level(v) - z) > 0.01) webkit_web_view_set_zoom_level(v, z);
    }
    if (t->win->tab() == t) t->win->update_chrome();
    if (ev == WEBKIT_LOAD_FINISHED && !t->reader) {
      Browser *b = t->win;
      later([b, t] {
        if (tab_alive(t)) b->fill_login(false);
      });
    }
    if (ev == WEBKIT_LOAD_FINISHED && !t->win->priv && !t->reader) { // private windows keep no history
      std::string uri = str(webkit_web_view_get_uri(v));
      if (fld::starts_with(uri, "http://") || fld::starts_with(uri, "https://") || fld::starts_with(uri, "file://")) {
        HistoryEntry e{time(nullptr), uri, str(webkit_web_view_get_title(v))};
        g_history.push_back(e);
        add_history(history_path(), e);
      }
    }
  }), t);
  // the title often arrives after the load finished: keep the history entry's title current
  g_signal_connect(v, "notify::title", CB([](GObject *o, GParamSpec *, gpointer) {
    WebKitWebView *v = WEBKIT_WEB_VIEW(o);
    std::string uri = str(webkit_web_view_get_uri(v)), title = str(webkit_web_view_get_title(v));
    if (!g_history.empty() && g_history.back().url == uri && g_history.back().title.empty() && !title.empty())
      g_history.back().title = title;
  }), t);

  g_signal_connect(v, "mouse-target-changed", CB([](WebKitWebView *, WebKitHitTestResult *hit, guint, gpointer d) {
    Tab *t = (Tab *)d;
    std::string link = webkit_hit_test_result_context_is_link(hit) ? str(webkit_hit_test_result_get_link_uri(hit)) : "";
    if (link != t->hover && t->win->tab() == t) t->win->set_status(link);
    t->hover = link;
    t->hit_link = link;
    t->hit_image = webkit_hit_test_result_context_is_image(hit) ? str(webkit_hit_test_result_get_image_uri(hit)) : "";
    if (t->hit_image.empty() && webkit_hit_test_result_context_is_media(hit))
      t->hit_image = str(webkit_hit_test_result_get_media_uri(hit));
    t->hit_editable = webkit_hit_test_result_context_is_editable(hit);
    t->hit_selection = webkit_hit_test_result_context_is_selection(hit);
  }), t);

  // window.open and target=_blank: a new tab next to this one
  g_signal_connect(v, "create", CB([](WebKitWebView *v, WebKitNavigationAction *, gpointer d) -> WebKitWebView * {
    Tab *t = (Tab *)d;
    Browser *b = t->win;
    int at = (int)(std::find(b->tabs.begin(), b->tabs.end(), t) - b->tabs.begin()) + 1;
    Tab *nt = b->new_tab("", false, v, at);
    g_signal_connect(nt->wv->view(), "ready-to-show", CB([](WebKitWebView *, gpointer d) {
      Tab *nt = (Tab *)d;
      Browser *b = nt->win;
      int i = (int)(std::find(b->tabs.begin(), b->tabs.end(), nt) - b->tabs.begin());
      if (i < (int)b->tabs.size()) b->select(i);
    }), nt);
    return WEBKIT_WEB_VIEW(g_object_ref(nt->wv->view()));
  }), t);
  g_signal_connect(v, "close", CB([](WebKitWebView *, gpointer d) {
    Tab *t = (Tab *)d;
    later([t] {
      if (!tab_alive(t)) return;
      Browser *b = t->win;
      b->close_tab((int)(std::find(b->tabs.begin(), b->tabs.end(), t) - b->tabs.begin()));
    });
  }), t);

  g_signal_connect(v, "decide-policy", CB([](WebKitWebView *, WebKitPolicyDecision *dec, WebKitPolicyDecisionType type, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
      WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(dec);
      if (!webkit_response_policy_decision_is_mime_type_supported(r)) {
        webkit_policy_decision_download(dec);
        return TRUE;
      }
      return FALSE;
    }
    WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(dec));
    std::string uri = str(webkit_uri_request_get_uri(webkit_navigation_action_get_request(a)));
    // other schemes (mailto:, magnet:, ...) go to their programs
    std::string scheme = fld::lower(uri.substr(0, uri.find(':')));
    static const char *ours[] = {"http", "https", "file", "about", "data", "blob", "javascript", "webkit"};
    if (uri.find(':') != std::string::npos && std::none_of(std::begin(ours), std::end(ours), [&](const char *s) { return scheme == s; })) {
      fld::spawn({"xdg-open", uri});
      webkit_policy_decision_ignore(dec);
      return TRUE;
    }
    // middle click or Ctrl+click on a link: a background tab
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
        webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
        (webkit_navigation_action_get_mouse_button(a) == 2 || (webkit_navigation_action_get_modifiers(a) & 1u /* Ctrl, in both input APIs */))) {
      Browser *b = t->win;
      int at = (int)(std::find(b->tabs.begin(), b->tabs.end(), t) - b->tabs.begin()) + 1;
      later([b, uri, at] {
        if (std::find(g_windows.begin(), g_windows.end(), b) != g_windows.end()) b->new_tab(uri, false, nullptr, at);
      });
      webkit_policy_decision_ignore(dec);
      return TRUE;
    }
    return FALSE;
  }), t);

  g_signal_connect(v, "script-dialog", CB([](WebKitWebView *, WebKitScriptDialog *dlg, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    webkit_script_dialog_ref(dlg);
    later([t, dlg] {
      std::string msg = fld::escape_at(str(webkit_script_dialog_get_message(dlg)));
      std::string host = url_host(t->uri);
      fl_message_title(host.empty() ? "Web" : host.c_str());
      switch (webkit_script_dialog_get_dialog_type(dlg)) {
        case WEBKIT_SCRIPT_DIALOG_ALERT: fl_message("%s", msg.c_str()); break;
        case WEBKIT_SCRIPT_DIALOG_CONFIRM:
          webkit_script_dialog_confirm_set_confirmed(dlg, fl_choice("%s", "Cancel", "OK", nullptr, msg.c_str()) == 1);
          break;
        case WEBKIT_SCRIPT_DIALOG_BEFORE_UNLOAD_CONFIRM:
          webkit_script_dialog_confirm_set_confirmed(
              dlg, fl_choice("This page asks you to confirm that you want to leave. Changes you made may not be saved.",
                             "Stay", "Leave", nullptr) == 1);
          break;
        case WEBKIT_SCRIPT_DIALOG_PROMPT: {
          const char *r = fl_input("%s", str(webkit_script_dialog_prompt_get_default_text(dlg)).c_str(), msg.c_str());
          if (r) webkit_script_dialog_prompt_set_text(dlg, r);
          break;
        }
      }
      webkit_script_dialog_close(dlg);
      webkit_script_dialog_unref(dlg);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "run-file-chooser", CB([](WebKitWebView *, WebKitFileChooserRequest *req, gpointer) -> gboolean {
    g_object_ref(req);
    later([req] {
      Fl_Native_File_Chooser fc;
      bool multi = webkit_file_chooser_request_get_select_multiple(req);
      fc.type(multi ? Fl_Native_File_Chooser::BROWSE_MULTI_FILE : Fl_Native_File_Chooser::BROWSE_FILE);
      fc.title(multi ? "Choose Files" : "Choose a File");
      if (fc.show() == 0 && fc.count() > 0) {
        std::vector<std::string> files;
        for (int i = 0; i < fc.count(); i++) files.push_back(fc.filename(i));
        std::vector<const char *> ptrs;
        for (auto &f : files) ptrs.push_back(f.c_str());
        ptrs.push_back(nullptr);
        webkit_file_chooser_request_select_files(req, ptrs.data());
      } else {
        webkit_file_chooser_request_cancel(req);
      }
      g_object_unref(req);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "authenticate", CB([](WebKitWebView *, WebKitAuthenticationRequest *req, gpointer) -> gboolean {
    g_object_ref(req);
    later([req] {
      Fl_Double_Window dlg(380, 190, "Sign In");
      std::string text = fld::escape_at("The site " + str(webkit_authentication_request_get_host(req)) +
                                        " asks for a user name and password" +
                                        (webkit_authentication_request_get_realm(req) && *webkit_authentication_request_get_realm(req)
                                             ? std::string(" (\"") + webkit_authentication_request_get_realm(req) + "\")."
                                             : std::string(".")));
      Fl_Box msg(12, 10, 356, 44, text.c_str());
      msg.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
      Fl_Input user(110, 62, 258, 28, "User name:");
      Fl_Secret_Input pass(110, 96, 258, 28, "Password:");
      Fl_Button cancel(176, 148, 90, 30, "Cancel");
      Fl_Return_Button ok(278, 148, 90, 30, "Sign In");
      int result = -1;
      cancel.callback([](Fl_Widget *, void *r) { *(int *)r = 0; }, &result);
      ok.callback([](Fl_Widget *, void *r) { *(int *)r = 1; }, &result);
      dlg.end();
      dlg.set_modal();
      dlg.show();
      while (result < 0 && dlg.shown()) Fl::wait();
      if (result == 1) {
        WebKitCredential *c = webkit_credential_new(user.value(), pass.value(), WEBKIT_CREDENTIAL_PERSISTENCE_FOR_SESSION);
        webkit_authentication_request_authenticate(req, c);
        webkit_credential_free(c);
      } else {
        webkit_authentication_request_cancel(req);
      }
      g_object_unref(req);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "load-failed-with-tls-errors", CB([](WebKitWebView *, char *uri, GTlsCertificate *cert, GTlsCertificateFlags, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    g_object_ref(cert);
    std::string u = str(uri);
    later([t, u, cert] {
      std::string host = url_host(u);
      int c = fl_choice("The connection to %s is not secure: its certificate is not trusted.\n\n"
                        "Someone may be trying to read or change what you send and receive.",
                        "Go Back", "Continue Anyway", nullptr, fld::escape_at(host).c_str());
      if (c == 1 && tab_alive(t)) {
        webkit_network_session_allow_tls_certificate_for_host(t->win->session, cert, host.c_str());
        webkit_web_view_load_uri(t->wv->view(), u.c_str());
      } else if (tab_alive(t) && webkit_web_view_can_go_back(t->wv->view())) {
        webkit_web_view_go_back(t->wv->view());
      }
      g_object_unref(cert);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "permission-request", CB([](WebKitWebView *, WebKitPermissionRequest *req, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    const char *what = WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req) ? "know your location"
                       : WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req) ? "show notifications"
                                                                        : nullptr;
    if (!what) {
      webkit_permission_request_deny(req);
      return TRUE;
    }
    g_object_ref(req);
    std::string host = url_host(t->uri), w = what;
    later([req, host, w] {
      if (fl_choice("%s wants to %s.", "Block", "Allow", nullptr, fld::escape_at(host).c_str(), w.c_str()) == 1)
        webkit_permission_request_allow(req);
      else webkit_permission_request_deny(req);
      g_object_unref(req);
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "show-notification", CB([](WebKitWebView *, WebKitNotification *n, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    fld::Notification note;
    note.app_name = "Web";
    note.summary = str(webkit_notification_get_title(n));
    note.body = str(webkit_notification_get_body(n));
    note.icon = "fl-web";
    note.actions = {{"default", "Open"}};
    g_object_ref(n);
    Browser *b = t->win;
    fld::notify(note, [n, t, b](const std::string &) {
      if (tab_alive(t)) {
        b->win->show();
        b->select((int)(std::find(b->tabs.begin(), b->tabs.end(), t) - b->tabs.begin()));
        webkit_notification_clicked(n);
      }
      g_object_unref(n);
      glib_loop_poke();
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "context-menu", CB([](WebKitWebView *, WebKitContextMenu *, WebKitHitTestResult *hit, gpointer d) -> gboolean {
    Tab *t = (Tab *)d;
    std::string link = webkit_hit_test_result_context_is_link(hit) ? str(webkit_hit_test_result_get_link_uri(hit)) : "";
    std::string image = webkit_hit_test_result_context_is_image(hit) ? str(webkit_hit_test_result_get_image_uri(hit)) : "";
    if (image.empty() && webkit_hit_test_result_context_is_media(hit)) image = str(webkit_hit_test_result_get_media_uri(hit));
    bool editable = webkit_hit_test_result_context_is_editable(hit);
    bool selection = webkit_hit_test_result_context_is_selection(hit);
    t->menu_shown = now_s();
    later([t, link, image, editable, selection] {
      if (tab_alive(t)) t->win->context_menu(t, link, image, editable, selection, Fl::event_x(), Fl::event_y());
    });
    return TRUE;
  }), t);

  g_signal_connect(v, "enter-fullscreen", CB([](WebKitWebView *, gpointer d) -> gboolean {
    ((Tab *)d)->win->set_fullscreen(true);
    return FALSE;
  }), t);
  g_signal_connect(v, "leave-fullscreen", CB([](WebKitWebView *, gpointer d) -> gboolean {
    ((Tab *)d)->win->set_fullscreen(false);
    return FALSE;
  }), t);

  g_signal_connect(v, "web-process-terminated", CB([](WebKitWebView *, WebKitWebProcessTerminationReason r, gpointer d) {
    Tab *t = (Tab *)d;
    if (r == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) return;
    later([t, r] {
      if (!tab_alive(t)) return;
      const char *why = r == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "used too much memory" : "crashed";
      if (fl_choice("The page %s.", "Close", "Reload", nullptr, why) == 1) webkit_web_view_reload(t->wv->view());
    });
  }), t);

  WebKitFindController *fc = webkit_web_view_get_find_controller(v);
  g_signal_connect(fc, "found-text", CB([](WebKitFindController *, guint n, gpointer d) {
    Tab *t = (Tab *)d;
    Browser *b = t->win;
    if (b->tab() != t) return;
    b->find_label = n >= 1000 ? "1000+ matches" : std::to_string(n) + (n == 1 ? " match" : " matches");
    b->find_count->label(b->find_label.c_str());
    b->find_input->color(fld::palette().bg2);
    b->findbar->redraw();
  }), t);
  g_signal_connect(fc, "failed-to-find-text", CB([](WebKitFindController *, gpointer d) {
    Tab *t = (Tab *)d;
    Browser *b = t->win;
    if (b->tab() != t) return;
    b->find_count->label("Not found");
    b->find_input->color(fl_color_average(FL_RED, fld::palette().bg2, 0.25f));
    b->findbar->redraw();
  }), t);
}

// A right button release in the page: WebKit asks for a menu with the
// context-menu signal; when it does not (some backends never do), the menu is
// built from the last hit test under the pointer.
void Browser::right_clicked(Tab *t, int x, int y) {
  double at = now_s();
  struct Pending {
    Tab *t;
    double at;
    int x, y;
  };
  Fl::add_timeout(0.25, [](void *d) {
    std::unique_ptr<Pending> p((Pending *)d);
    if (!tab_alive(p->t) || p->t->menu_shown >= p->at - 0.5) return;
    Tab *t = p->t;
    t->win->context_menu(t, t->hit_link, t->hit_image, t->hit_editable, t->hit_selection, p->x, p->y);
  }, new Pending{t, at, x, y});
}

void Browser::context_menu(Tab *t, const std::string &link, const std::string &image, bool editable, bool selection,
                           int x, int y) {
  WebKitWebView *v = t->wv->view();
  auto edit = [v](const char *cmd) {
    return [v, cmd] {
      webkit_web_view_execute_editing_command(v, cmd);
      glib_loop_poke();
    };
  };
  fld::Menu m;
  if (!link.empty()) {
    m.item("Open Link in New Tab", [this, link] { new_tab(link, false, nullptr, cur + 1); });
    m.item("Open Link in New Window", [link, this] { new_window({link}, priv); });
    m.item("Open Link in Private Window", [link] { new_window({link}, true); });
    m.item("Copy Link Address", [link] { Fl::copy(link.c_str(), (int)link.size(), 1); });
    m.item("Download Link", [v, link] { webkit_web_view_download_uri(v, link.c_str()); }).divider();
  }
  if (!image.empty()) {
    m.item("Open Image in New Tab", [this, image] { new_tab(image, false, nullptr, cur + 1); });
    m.item("Copy Image Address", [image] { Fl::copy(image.c_str(), (int)image.size(), 1); });
    m.item("Save Image", [v, image] { webkit_web_view_download_uri(v, image.c_str()); }).divider();
  }
  if (editable) {
    m.item("Undo", edit(WEBKIT_EDITING_COMMAND_UNDO), 0, FL_CTRL + 'z');
    m.item("Redo", edit(WEBKIT_EDITING_COMMAND_REDO), 0, FL_CTRL + FL_SHIFT + 'z').divider();
    m.item("Cut", edit(WEBKIT_EDITING_COMMAND_CUT), 0, FL_CTRL + 'x');
    m.item("Copy", edit(WEBKIT_EDITING_COMMAND_COPY), 0, FL_CTRL + 'c');
    m.item("Paste", [t] {
      // the system clipboard reaches the page through a Ctrl+V
      t->wv->take_focus();
      t->wv->paste_clipboard();
    }, 0, FL_CTRL + 'v');
    m.item("Select All", edit(WEBKIT_EDITING_COMMAND_SELECT_ALL), 0, FL_CTRL + 'a').divider();
  } else if (selection) {
    m.item("Copy", edit(WEBKIT_EDITING_COMMAND_COPY), 0, FL_CTRL + 'c');
    m.divider();
  }
  if (link.empty() && image.empty() && !editable) {
    m.item("Back", [this] { go(-1); }, webkit_web_view_can_go_back(v) ? 0 : FL_MENU_INACTIVE, FL_ALT + FL_Left);
    m.item("Forward", [this] { go(1); }, webkit_web_view_can_go_forward(v) ? 0 : FL_MENU_INACTIVE, FL_ALT + FL_Right);
    m.item("Reload", [this] { reload_or_stop(); }, 0, FL_F + 5).divider();
    m.item("Select All", edit(WEBKIT_EDITING_COMMAND_SELECT_ALL), 0, FL_CTRL + 'a');
    m.item(is_bookmarked(t->uri) ? "Remove Bookmark" : "Bookmark This Page", [this] { toggle_bookmark(); }, 0,
           FL_CTRL + 'd');
    m.item("Copy Page Address", [t] { Fl::copy(t->uri.c_str(), (int)t->uri.size(), 1); });
    m.item("Save Page As…", [this] { save_page(); }, t->uri.empty() ? FL_MENU_INACTIVE : 0, FL_CTRL + 's');
    m.item("View Page Source", [this] { view_source(); }, t->uri.empty() ? FL_MENU_INACTIVE : 0, FL_CTRL + 'u');
    m.divider();
    m.toggle("Full Screen", fullscreen, [this] { set_fullscreen(!fullscreen); }, 0);
  }
  if (g_inspector_port) m.divider().item("Inspect (Developer Tools)", [this] { open_devtools(); }, 0, FL_F + 12);
  m.popup(x, y);
  glib_loop_poke();
}

void Browser::tab_menu(int i) {
  fld::Menu m;
  if (i < 0 || i >= (int)tabs.size()) {
    m.item("New Tab", [this] { new_tab("", true); }, 0, FL_CTRL + 't');
    m.item("Reopen Closed Tab", [this] { reopen_closed(); }, closed.empty() ? FL_MENU_INACTIVE : 0,
           FL_CTRL + FL_SHIFT + 't');
    m.popup();
    return;
  }
  Tab *t = tabs[(size_t)i];
  int n = (int)tabs.size();
  m.item("New Tab", [this] { new_tab("", true); }, 0, FL_CTRL + 't').divider();
  m.item("Reload", [t] {
    webkit_web_view_reload(t->wv->view());
    glib_loop_poke();
  });
  m.item("Duplicate", [this, t] {
    int k = index_of(t);
    if (k >= 0 && !t->uri.empty()) new_tab(t->uri, true, nullptr, k + 1);
  }, t->uri.empty() ? FL_MENU_INACTIVE : 0);
  m.item(is_bookmarked(t->uri) ? "Remove Bookmark" : "Bookmark", [this, t] {
    int k = index_of(t);
    if (k < 0) return;
    if (k != cur) select(k);
    toggle_bookmark();
  }, t->uri.empty() ? FL_MENU_INACTIVE : 0);
  m.item("Copy Address", [t] { Fl::copy(t->uri.c_str(), (int)t->uri.size(), 1); }, t->uri.empty() ? FL_MENU_INACTIVE : 0)
      .divider();
  m.item("Move Tab Left", [this, i] {
    fld::move_element(tabs, i, i - 1);
    cur = fld::moved_index(cur, i, i - 1);
    refresh_tabs();
  }, i > 0 ? 0 : FL_MENU_INACTIVE);
  m.item("Move Tab Right", [this, i] {
    fld::move_element(tabs, i, i + 1);
    cur = fld::moved_index(cur, i, i + 1);
    refresh_tabs();
  }, i + 1 < n ? 0 : FL_MENU_INACTIVE);
  m.item("Move to New Window", [this, i] { later_detach(i); }, n > 1 ? 0 : FL_MENU_INACTIVE).divider();
  m.item("Close Tab", [this, i] { later_close(i); }, 0, FL_CTRL + 'w');
  m.item("Close Other Tabs", [this, t] {
    for (int k = (int)tabs.size() - 1; k >= 0; k--)
      if (tabs[(size_t)k] != t) later_close(k);
  }, n > 1 ? 0 : FL_MENU_INACTIVE);
  m.item("Close Tabs to the Right", [this, i] {
    for (int k = (int)tabs.size() - 1; k > i; k--) later_close(k);
  }, i + 1 < n ? 0 : FL_MENU_INACTIVE);
  m.item("Reopen Closed Tab", [this] { reopen_closed(); }, closed.empty() ? FL_MENU_INACTIVE : 0,
         FL_CTRL + FL_SHIFT + 't');
  m.popup();
}

// The main menu and the menu bar share the commands (ids) and labels.
enum Cmd {
  C_NEW_TAB = 1, C_NEW_WINDOW, C_OPEN_FILE, C_FIND, C_ZOOM_IN, C_ZOOM_OUT, C_ZOOM_RESET, C_BOOKMARK, C_BOOKMARKS,
  C_HISTORY, C_DOWNLOADS, C_SETTINGS, C_ABOUT, C_CLOSE_WINDOW, C_QUIT, C_PRIVATE, C_FULLSCREEN, C_DEVTOOLS,
  C_MENUBAR, C_SAVE_PAGE, C_SOURCE, C_REOPEN, C_CLOSE_TAB, C_RELOAD, C_BACK, C_FORWARD, C_HOME, C_UNDO, C_REDO,
  C_CUT, C_COPY, C_PASTE, C_SELECT_ALL, C_ADBLOCK_SITE, C_READER, C_PASSWORDS, C_FILL_LOGIN,
  C_BOOKMARK_BASE = 1000,
};

// a site in the allowed list (example.com covers www.example.com too)
static bool host_matches(const std::string &host, const std::string &site) {
  return host == site || (host.size() > site.size() && host.compare(host.size() - site.size(), site.size(), site) == 0 &&
                          host[host.size() - site.size() - 1] == '.');
}

static bool site_allowed(const std::string &uri) {
  std::string host = url_host(uri);
  if (host.empty()) return false;
  for (auto &a : g_cfg.adblock_allow)
    if (host_matches(host, a)) return true;
  return false;
}

// "Block Ads on This Site": the site joins or leaves the allowed sites; the
// page reloads once the filter is rebuilt
static void toggle_site_blocking(Tab *t) {
  if (!t) return;
  std::string host = url_host(t->uri);
  if (host.empty()) return;
  auto &allow = g_cfg.adblock_allow;
  if (site_allowed(t->uri)) {
    allow.erase(std::remove_if(allow.begin(), allow.end(), [&](const std::string &a) { return host_matches(host, a); }),
                allow.end());
  } else {
    if (host.compare(0, 4, "www.") == 0) host = host.substr(4);
    allow.push_back(host);
  }
  g_cfg.save();
  web::adblock::apply(g_cfg.adblock(), [t] {
    if (tab_alive(t)) webkit_web_view_reload(t->wv->view());
    glib_loop_poke();
  });
}

void Browser::command(long id) {
  WebKitWebView *v = view();
  auto edit = [v](const char *c) {
    if (v) webkit_web_view_execute_editing_command(v, c);
  };
  switch (id) {
    case C_NEW_TAB: new_tab("", true); break;
    case C_NEW_WINDOW: new_window({}, priv); break;
    case C_PRIVATE: new_window({}, true); break;
    case C_OPEN_FILE: open_file(); break;
    case C_FIND: find_open(); break;
    case C_ZOOM_IN: zoom(1); break;
    case C_ZOOM_OUT: zoom(-1); break;
    case C_ZOOM_RESET: zoom(0); break;
    case C_BOOKMARK: toggle_bookmark(); break;
    case C_BOOKMARKS: show_bookmarks(this); break;
    case C_HISTORY: show_history(this); break;
    case C_DOWNLOADS: show_downloads(); break;
    case C_SETTINGS: show_settings(); break;
    case C_ABOUT: fld::about_dialog("Web", "fl-web", "A web browser built with FLTK and WPE WebKit."); break;
    case C_CLOSE_WINDOW: win->hide(); break;
    case C_QUIT: for (auto *w : std::vector<Browser *>(g_windows)) w->win->hide(); break;
    case C_FULLSCREEN: set_fullscreen(!fullscreen); break;
    case C_DEVTOOLS: open_devtools(); break;
    case C_MENUBAR:
      g_cfg.menubar = !g_cfg.menubar;
      g_cfg.save();
      for (auto *w : g_windows) w->layout();
      break;
    case C_SAVE_PAGE: save_page(); break;
    case C_SOURCE: view_source(); break;
    case C_REOPEN: reopen_closed(); break;
    case C_CLOSE_TAB: later_close(cur); break;
    case C_RELOAD: reload_or_stop(); break;
    case C_BACK: go(-1); break;
    case C_FORWARD: go(1); break;
    case C_HOME: load(g_cfg.home); break;
    case C_UNDO: edit(WEBKIT_EDITING_COMMAND_UNDO); break;
    case C_REDO: edit(WEBKIT_EDITING_COMMAND_REDO); break;
    case C_CUT: edit(WEBKIT_EDITING_COMMAND_CUT); break;
    case C_COPY: edit(WEBKIT_EDITING_COMMAND_COPY); break;
    case C_PASTE:
      if (tab()) tab()->wv->paste_clipboard();
      break;
    case C_SELECT_ALL: edit(WEBKIT_EDITING_COMMAND_SELECT_ALL); break;
    case C_ADBLOCK_SITE: toggle_site_blocking(tab()); break;
    case C_READER: toggle_reader(); break;
    case C_PASSWORDS: show_passwords(); break;
    case C_FILL_LOGIN: fill_login(true); break;
    default:
      if (id >= C_BOOKMARK_BASE && id - C_BOOKMARK_BASE < (long)g_bookmarks.size())
        load(g_bookmarks[(size_t)(id - C_BOOKMARK_BASE)].url);
  }
  glib_loop_poke();
}

// for_bar: the menu bar's layout (File, Edit, View, ...); otherwise the
// toolbar's menu button
void Browser::build_menu(fld::Menu &m, bool for_bar) {
  auto cmd = [this](long id) { return [this, id] { command(id); }; };
  auto adblock_item = [&](fld::Menu &mm) {
    std::string uri = tab() ? tab()->uri : "";
    bool web = uri.compare(0, 7, "http://") == 0 || uri.compare(0, 8, "https://") == 0;
    mm.toggle("Block Ads on This Site", g_cfg.block_ads && web && !site_allowed(uri), cmd(C_ADBLOCK_SITE),
              g_cfg.block_ads && web ? 0 : FL_MENU_INACTIVE);
  };
  auto reader_item = [&](fld::Menu &mm) {
#ifdef FLD_HAVE_READER
    bool web = tab() && (tab()->reader || !login_origin(tab()->uri).empty() || fld::starts_with(tab()->uri, "file://"));
    mm.toggle("Reader Mode", tab() && tab()->reader, cmd(C_READER), web ? 0 : FL_MENU_INACTIVE);
#else
    (void)mm;
#endif
  };
  auto password_items = [&](fld::Menu &mm) {
#ifdef FLD_HAVE_SECRET
    mm.item("Fill Saved Login", cmd(C_FILL_LOGIN), tab() && !login_origin(tab()->uri).empty() ? 0 : FL_MENU_INACTIVE);
    mm.item("Saved Passwords…", cmd(C_PASSWORDS));
#else
    (void)mm;
#endif
  };
  auto bookmarks = [&]() {
    bool marked = tab() && is_bookmarked(tab()->uri);
    m.item(marked ? "Remove Bookmark" : "Bookmark This Page", cmd(C_BOOKMARK), 0, FL_CTRL + 'd');
    m.item("Manage Bookmarks…", cmd(C_BOOKMARKS), 0, FL_CTRL + FL_SHIFT + 'o');
    if (!g_bookmarks.empty()) m.divider();
    for (size_t i = 0; i < g_bookmarks.size() && i < 30; i++) {
      std::string l = g_bookmarks[i].title.empty() ? g_bookmarks[i].url : g_bookmarks[i].title;
      if (l.size() > 60) l = l.substr(0, 57) + "…";
      m.item(fld::Menu::text(l), cmd(C_BOOKMARK_BASE + (long)i));
    }
  };
  if (!for_bar) {
    m.item("New Tab", cmd(C_NEW_TAB), 0, FL_CTRL + 't');
    m.item("New Window", cmd(C_NEW_WINDOW), 0, FL_CTRL + 'n');
    m.item("New Private Window", cmd(C_PRIVATE), 0, FL_CTRL + FL_SHIFT + 'n');
    m.item("Open File…", cmd(C_OPEN_FILE), 0, FL_CTRL + 'o');
    m.item("Save Page As…", cmd(C_SAVE_PAGE), tab() && !tab()->uri.empty() ? 0 : FL_MENU_INACTIVE, FL_CTRL + 's');
    m.divider();
    m.item("Find in Page…", cmd(C_FIND), 0, FL_CTRL + 'f');
    m.item("Zoom In", cmd(C_ZOOM_IN), 0, FL_CTRL + '=');
    m.item("Zoom Out", cmd(C_ZOOM_OUT), 0, FL_CTRL + '-');
    m.item("Actual Size", cmd(C_ZOOM_RESET), 0, FL_CTRL + '0');
    m.toggle("Full Screen", fullscreen, cmd(C_FULLSCREEN));
    m.toggle("Show Menu Bar", g_cfg.menubar, cmd(C_MENUBAR));
    reader_item(m);
    adblock_item(m);
    m.divider();
    m.submenu("Bookmarks");
    bookmarks();
    m.end_submenu();
    m.item("History…", cmd(C_HISTORY), 0, FL_CTRL + 'h');
    m.item("Downloads…", cmd(C_DOWNLOADS), 0, FL_CTRL + 'j');
    password_items(m);
    m.divider();
    m.item("Developer Tools", cmd(C_DEVTOOLS), 0, FL_F + 12);
    m.item("View Page Source", cmd(C_SOURCE), tab() && !tab()->uri.empty() ? 0 : FL_MENU_INACTIVE, FL_CTRL + 'u');
    m.item("Settings…", cmd(C_SETTINGS));
    m.item("About Web", cmd(C_ABOUT)).divider();
    m.item("Close Window", cmd(C_CLOSE_WINDOW), 0, FL_CTRL + FL_SHIFT + 'w');
    m.item("Quit", cmd(C_QUIT), 0, FL_CTRL + 'q');
    return;
  }
  m.submenu("&File");
  m.item("New Tab", cmd(C_NEW_TAB), 0, FL_CTRL + 't');
  m.item("New Window", cmd(C_NEW_WINDOW), 0, FL_CTRL + 'n');
  m.item("New Private Window", cmd(C_PRIVATE), 0, FL_CTRL + FL_SHIFT + 'n').divider();
  m.item("Open File…", cmd(C_OPEN_FILE), 0, FL_CTRL + 'o');
  m.item("Save Page As…", cmd(C_SAVE_PAGE), 0, FL_CTRL + 's').divider();
  m.item("Reopen Closed Tab", cmd(C_REOPEN), closed.empty() ? FL_MENU_INACTIVE : 0, FL_CTRL + FL_SHIFT + 't');
  m.item("Close Tab", cmd(C_CLOSE_TAB), 0, FL_CTRL + 'w');
  m.item("Close Window", cmd(C_CLOSE_WINDOW), 0, FL_CTRL + FL_SHIFT + 'w');
  m.item("Quit", cmd(C_QUIT), 0, FL_CTRL + 'q');
  m.end_submenu();
  m.submenu("&Edit");
  m.item("Undo", cmd(C_UNDO), 0, FL_CTRL + 'z');
  m.item("Redo", cmd(C_REDO), 0, FL_CTRL + FL_SHIFT + 'z').divider();
  m.item("Cut", cmd(C_CUT), 0, FL_CTRL + 'x');
  m.item("Copy", cmd(C_COPY), 0, FL_CTRL + 'c');
  m.item("Paste", cmd(C_PASTE), 0, FL_CTRL + 'v');
  m.item("Select All", cmd(C_SELECT_ALL), 0, FL_CTRL + 'a').divider();
  m.item("Find in Page…", cmd(C_FIND), 0, FL_CTRL + 'f').divider();
  password_items(m);
  m.divider();
  m.item("Settings…", cmd(C_SETTINGS));
  m.end_submenu();
  m.submenu("&View");
  m.item("Reload", cmd(C_RELOAD), 0, FL_F + 5).divider();
  m.item("Zoom In", cmd(C_ZOOM_IN), 0, FL_CTRL + '=');
  m.item("Zoom Out", cmd(C_ZOOM_OUT), 0, FL_CTRL + '-');
  m.item("Actual Size", cmd(C_ZOOM_RESET), 0, FL_CTRL + '0').divider();
  m.toggle("Full Screen", fullscreen, cmd(C_FULLSCREEN));
  m.toggle("Show Menu Bar", g_cfg.menubar, cmd(C_MENUBAR));
  reader_item(m);
  adblock_item(m);
  m.divider();
  m.item("View Page Source", cmd(C_SOURCE), 0, FL_CTRL + 'u');
  m.item("Developer Tools", cmd(C_DEVTOOLS), 0, FL_F + 12);
  m.end_submenu();
  m.submenu("&History");
  m.item("Back", cmd(C_BACK), 0, FL_ALT + FL_Left);
  m.item("Forward", cmd(C_FORWARD), 0, FL_ALT + FL_Right);
  m.item("Home", cmd(C_HOME), 0, FL_ALT + FL_Home).divider();
  m.item("Show History…", cmd(C_HISTORY), 0, FL_CTRL + 'h');
  m.item("Downloads…", cmd(C_DOWNLOADS), 0, FL_CTRL + 'j');
  m.end_submenu();
  m.submenu("&Bookmarks");
  bookmarks();
  m.end_submenu();
  m.submenu("He&lp");
  m.item("About Web", cmd(C_ABOUT));
  m.end_submenu();
}

// The menu bar is built again each time it opens (bookmarks, toggles)
class WebMenuBar : public fld::MenuBar {
public:
  Browser *b;
  fld::Menu menu;
  WebMenuBar(int x, int y, int w, int h, Browser *br) : fld::MenuBar(x, y, w, h), b(br) { rebuild(); }
  void rebuild() {
    menu = fld::Menu();
    b->build_menu(menu, true);
    const Fl_Menu_Item *mi = menu.items();
    menu_items_.assign(mi, mi + count_items(mi));
    // the actions run through the callback; the ids are the actions' ids
    for (auto &it : menu_items_)
      if (it.text && !(it.flags & FL_SUBMENU)) it.callback_ = cb;
    Fl_Menu_Bar::menu(menu_items_.data());
  }
  int handle(int e) override {
    if (e == FL_PUSH) rebuild();
    // shortcuts are the browser's own (they also work with the bar hidden)
    if (e == FL_SHORTCUT) return 0;
    return fld::MenuBar::handle(e);
  }

private:
  std::vector<Fl_Menu_Item> menu_items_;
  static int count_items(const Fl_Menu_Item *m) {
    int n = 0, depth = 0;
    for (;; n++) {
      if (!m[n].text) {
        if (depth == 0) break;
        depth--;
        continue;
      }
      if (m[n].flags & FL_SUBMENU) depth++;
    }
    return n + 1;
  }
  static void cb(Fl_Widget *w, void *d) {
    WebMenuBar *bar = (WebMenuBar *)w;
    bar->menu.run((long)d);
  }
};

Fl_Menu_Bar *Browser::make_menubar(int W) {
  Fl_Menu_Bar *m = new WebMenuBar(0, 0, W, menu_h(), this);
  m->box(FL_FLAT_BOX);
  m->hide();
  return m;
}

// Save Page As: the page as WebKit has it, into a file the user chooses
void Browser::save_page() {
  Tab *t = tab();
  if (!t || t->uri.empty()) return;
  Fl_Native_File_Chooser fc;
  fc.title("Save Page As");
  fc.type(Fl_Native_File_Chooser::BROWSE_SAVE_FILE);
  fc.options(Fl_Native_File_Chooser::SAVEAS_CONFIRM);
  std::string name = fallback_title(t->uri);
  for (char &c : name)
    if (c == '/' || c == ':') c = '_';
  fc.preset_file((t->title.empty() ? name : t->title).c_str());
  fc.directory(fld::user_dir("DOWNLOAD").c_str());
  fc.filter("Web archive (MHTML)\t*.mhtml\nAll files\t*");
  if (fc.show() != 0 || !fc.filename()) return;
  std::string path = fc.filename();
  if (fld::extension(path).empty()) path += ".mhtml";
  GFile *f = g_file_new_for_path(path.c_str());
  webkit_web_view_save_to_file(t->wv->view(), f, WEBKIT_SAVE_MODE_MHTML, nullptr,
                               [](GObject *o, GAsyncResult *r, gpointer d) {
                                 GError *e = nullptr;
                                 webkit_web_view_save_to_file_finish(WEBKIT_WEB_VIEW(o), r, &e);
                                 std::string *p = (std::string *)d;
                                 if (e) {
                                   std::string msg = e->message;
                                   g_error_free(e);
                                   later([msg] { fl_alert("Could not save the page: %s", fld::escape_at(msg).c_str()); });
                                 }
                                 delete p;
                               },
                               new std::string(path));
  g_object_unref(f);
  glib_loop_poke();
}

// View Page Source: the main resource's data, opened in the text editor
void Browser::view_source() {
  Tab *t = tab();
  if (!t || t->uri.empty()) return;
  WebKitWebResource *res = webkit_web_view_get_main_resource(t->wv->view());
  if (!res) return;
  std::string name = fallback_title(t->uri);
  for (char &c : name)
    if (!isalnum((unsigned char)c) && c != '.' && c != '-') c = '_';
  webkit_web_resource_get_data(res, nullptr, [](GObject *o, GAsyncResult *r, gpointer d) {
    std::unique_ptr<std::string> name((std::string *)d);
    gsize len = 0;
    GError *e = nullptr;
    guchar *data = webkit_web_resource_get_data_finish(WEBKIT_WEB_RESOURCE(o), r, &len, &e);
    if (!data) {
      if (e) g_error_free(e);
      return;
    }
    std::string dir = fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-web-source";
    fld::mkdir_p(dir, 0700);
    std::string path = dir + "/" + *name + ".html";
    fld::write_file(path, std::string((const char *)data, len));
    g_free(data);
    later([path] {
      if (!fld::spawn({"fl-edit", path})) fld::spawn({"xdg-open", path});
    });
  }, new std::string(name));
  glib_loop_poke();
}
