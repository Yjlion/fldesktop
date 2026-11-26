// fl-files: network locations (GVfs)
#include "files_app.h"

// ---------------------------------------------------------------------------
// Network locations (GVfs)

// Mounts a location (smb://server/share...) and opens its folder here, in a
// new window or in a new tab
void mount_and_go(const std::string &uri, int where) {
#ifdef FLD_HAVE_GVFS
  if (gv::uri_scheme(uri) == "network") {
    if (where == 2) new_tab(NETWORK_URI);
    else if (where == 1) fld::spawn({"fl-files", NETWORK_URI});
    else navigate(NETWORK_URI);
    return;
  }
  if (where == 1) {
    fld::spawn({"fl-files", uri});
    return;
  }
  std::string name = gv::display_name_for_uri(uri);
  g_status->copy_label(fld::escape_at("Connecting to " + name + "…").c_str());
  gv::Client::get().mount_uri(uri, [uri, name, where](const std::string &err, const std::string &path) {
    update_status();
    if (err == gv::kCancelled) return;
    if (!err.empty()) {
      fl_alert("Could not open %s:\n%s", fld::escape_at(uri).c_str(), fld::escape_at(err).c_str());
      return;
    }
    gv::Client::get().refresh();
    if (where == 2) new_tab(path);
    else navigate(path);
  });
#else
  (void)where;
  fl_alert("fl-files was built without GVfs: it cannot open %s.", fld::escape_at(uri).c_str());
#endif
}

void connect_to_server() {
#ifdef FLD_HAVE_GVFS
  fld::Ini c;
  c.load(settings_path());
  std::vector<std::string> recent;
  for (int i = 0; i < 10; i++) {
    std::string r = c.get("Servers", "Recent" + std::to_string(i), "");
    if (!r.empty()) recent.push_back(r);
  }
  bool bookmark = false;
  std::string uri = connect_dialog(recent, bookmark);
  if (!uri.empty() && !gv::looks_like_uri(uri)) {
    fl_alert("\"%s\" is not a server address.\nIt starts with the kind of server, for example smb://, sftp:// or ftp://.",
             fld::escape_at(uri).c_str());
    uri.clear();
  }
  if (!uri.empty()) {
    recent.erase(std::remove(recent.begin(), recent.end(), uri), recent.end());
    recent.insert(recent.begin(), uri);
  }
  c.remove_group("Servers");
  for (size_t i = 0; i < recent.size() && i < 10; i++) c.set("Servers", "Recent" + std::to_string(i), recent[i]);
  c.save(settings_path());
  if (uri.empty()) return;
  if (bookmark) add_bookmark_uri(uri, gv::display_name_for_uri(uri));
  mount_and_go(uri);
#else
  fl_alert("fl-files was built without GVfs support.");
#endif
}


int global_keys(int e) {
  if (e != FL_SHORTCUT) return 0;
  int k = Fl::event_key();
  int st = Fl::event_state();
  bool ctrl = st & FL_CTRL, alt = st & FL_ALT, shift = st & FL_SHIFT;
  if (alt && k == FL_Left) { go_back(); return 1; }
  if (alt && k == FL_Right) { go_forward(); return 1; }
  if (alt && k == FL_Up) { go_up(); return 1; }
  if (alt && k == FL_Home) { navigate(fld::home_dir()); return 1; }
  if (k == FL_F + 9) { show_tree(!S.show_tree); return 1; }
  if (k == FL_F + 4) { toggle_split(); return 1; }
  if (k == FL_F + 3 || (ctrl && shift && (k == 'f' || k == 'F'))) { show_search_bar(true); return 1; }
  if (k == FL_Escape && in_search() && Fl::focus() == g_view) { show_search_bar(false); return 1; }
  if (k == FL_BackSpace && Fl::focus() == g_view) { go_up(); return 1; }
  if (k == FL_F + 5) { refresh(); return 1; }
  // tabs: Alt+1..9, Ctrl+Tab, Ctrl+Page Up/Down
  if (alt && !ctrl && k >= '1' && k <= '9') {
    int i = k == '9' ? (int)g_tabs.size() - 1 : k - '1';
    if (i < (int)g_tabs.size()) activate_tab(i);
    return 1;
  }
  if (ctrl && (k == FL_Tab || k == FL_Page_Up || k == FL_Page_Down) && g_tabs.size() > 1) {
    int n = (int)g_tabs.size(), d = (k == FL_Page_Up || (k == FL_Tab && shift)) ? -1 : 1;
    activate_tab((g_cur + d + n) % n);
    return 1;
  }
  if (!ctrl) return 0;
  switch (k) {
    case 'l': edit_location(); return 1;
    case 'm': toggle_menubar(); return 1;
    case 'f': g_filter->take_focus(); return 1;
    case 'h': toggle_hidden(); return 1;
    case 'd': if (!in_search()) add_bookmark(g_cwd); return 1;
    case ',': show_preferences(); return 1;
    case 'n':
      if (shift) new_item(true);
      else fld::spawn({"fl-files", in_trash() ? fld::home_dir() : g_cwd});
      return 1;
    case 't': new_tab(in_trash() || in_search() ? fld::home_dir() : g_cwd); return 1;
    case 'w': close_tab(g_cur); return 1;
    case 'q': g_win->hide(); return 1;
    case '1': set_view(VIEW_ICONS); return 1;
    case '2': set_view(VIEW_COMPACT); return 1;
    case '3': set_view(VIEW_DETAILS); return 1;
    case '=': case '+': zoom(1); return 1;
    case '-': zoom(-1); return 1;
    case '0': zoom(0); return 1;
  }
  if (Fl::focus() != g_view) return 0;
  switch (k) {
    case 'a': select_all(); return 1;
    case 'c': copy_to_clipboard(false); return 1;
    case 'x': copy_to_clipboard(true); return 1;
    case 'v': if (!in_search()) paste_into(g_cwd); return 1;
    case 'z': undo(); return 1;
  }
  return 0;
}
