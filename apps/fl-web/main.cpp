#include "web_app.h"

// ---------------------------------------------------------------------------
// Settings and shared state

Config g_cfg;

static std::string data_dir() { return fld::data_home() + "/fl-web"; }
std::string bookmarks_path() { return data_dir() + "/bookmarks.tsv"; }
std::string history_path() { return data_dir() + "/history.tsv"; }
static std::string session_path() { return data_dir() + "/session.txt"; }
// the zoom chosen for each site ([Zoom] host=1.25); 100 % is not stored
static std::string zoom_path() { return data_dir() + "/zoom.ini"; }

double site_zoom(const std::string &uri) {
  std::string host = url_host(uri);
  if (host.empty()) return 1.0;
  static fld::Ini ini;
  static time_t loaded = -1;
  struct stat st;
  time_t m = stat(zoom_path().c_str(), &st) == 0 ? st.st_mtime : 0;
  if (m != loaded) {
    ini = fld::Ini();
    ini.load(zoom_path());
    loaded = m;
  }
  double z = ini.get_double("Zoom", host, 1.0);
  return z >= 0.3 && z <= 3.0 ? z : 1.0;
}

void remember_site_zoom(const std::string &uri, double z) {
  std::string host = url_host(uri);
  if (host.empty()) return;
  fld::Ini ini;
  ini.load(zoom_path());
  if (z > 0.99 && z < 1.01) ini.remove("Zoom", host);
  else {
    char buf[16];
    snprintf(buf, sizeof buf, "%.2f", z);
    ini.set("Zoom", host, buf);
  }
  ini.save(zoom_path());
}

WebKitNetworkSession *g_session;
static WebKitNetworkSession *g_private_session; // private windows: nothing is stored
int g_inspector_port = 0;                // WebKit's inspector HTTP server (F12)
WebKitSettings *g_settings;
WebKitUserContentManager *g_ucm;
std::vector<Bookmark> g_bookmarks;
std::vector<HistoryEntry> g_history;

std::vector<std::unique_ptr<Download>> g_downloads;

bool is_bookmarked(const std::string &url) {
  return std::any_of(g_bookmarks.begin(), g_bookmarks.end(), [&](const Bookmark &b) { return b.url == url; });
}

std::vector<Browser *> g_windows;

// ---------------------------------------------------------------------------
// Windows and sessions

WebKitNetworkSession *private_session() {
  if (!g_private_session) {
    g_private_session = webkit_network_session_new_ephemeral();
    g_signal_connect(g_private_session, "download-started", G_CALLBACK(download_started), nullptr);
  }
  return g_private_session;
}

Browser *new_window(const std::vector<std::string> &urls, bool priv) {
  Browser *b = new Browser(priv);
  if (urls.empty()) b->new_tab(priv ? "" : g_cfg.home, true);
  for (auto &u : urls) b->new_tab(u, b->tabs.empty());
  b->win->show();
  b->layout();
  return b;
}

// the open tabs, but not those of private windows (they leave no trace)
// nor the developer tools
std::vector<std::string> session_uris() {
  std::vector<std::string> out;
  for (auto *w : g_windows) {
    if (w->priv) continue;
    for (auto *t : w->tabs)
      if (!t->uri.empty() && !fld::starts_with(t->uri, "http://127.0.0.1:" + std::to_string(g_inspector_port) + "/"))
        out.push_back(one_line(t->uri));
  }
  return out;
}

void save_session() {
  std::string data;
  for (auto &u : session_uris()) data += u + "\n";
  fld::mkdir_p(data_dir());
  fld::write_file(session_path(), data);
}

// a free TCP port on 127.0.0.1 (0 when there is none)
static int free_loopback_port() {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return 0;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = 0;
  socklen_t len = sizeof a;
  int port = 0;
  if (bind(fd, (sockaddr *)&a, sizeof a) == 0 && getsockname(fd, (sockaddr *)&a, &len) == 0) port = ntohs(a.sin_port);
  close(fd);
  return port;
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-web [--private] [--new-window] [URL or file...]\n"
             "       fl-web --adblock-convert LIST... (prints WebKit content blocker rules)\n");
      return 0;
    }
    if (a == "--adblock-convert") {
      std::vector<std::string> files(argv + i + 1, argv + argc);
      size_t rules = 0, skipped = 0;
      std::string json = web::adblock::convert_files(files, &rules, &skipped);
      printf("%s\n", json.c_str());
      fprintf(stderr, "%zu rules, %zu filters skipped\n", rules, skipped);
      return rules ? 0 : 1;
    }
    if (a == "-v" || a == "--version") {
      // the rendering (GPU or CPU) can only be told with a display
      const char *mode = getenv("WAYLAND_DISPLAY") ? web::render_mode() : "no display";
      printf("fl-web %s (WPE WebKit %u.%u.%u, %s, rendering: %s)\n", FLD_VERSION, webkit_get_major_version(),
             webkit_get_minor_version(), webkit_get_micro_version(), backend_name(), mode);
      return 0;
    }
  }
  fld::init_app("fl-web", argc, argv);
  Fl::lock(); // notifications report back with Fl::awake
  std::vector<std::string> urls;
  bool priv = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--private" || a == "--incognito") { priv = true; continue; }
    if (a == "--new-window" || a.empty()) continue;
    if (fld::file_exists(a) && a.find("://") == std::string::npos) a = "file://" + fld::uri_encode_path(fld::abs_path(a));
    urls.push_back(url_from_input(a, ""));
  }
  g_cfg.load();
  // the developer tools: WebKit serves its inspector on a loopback port; the
  // variable must be set before WebKit starts
  if (g_cfg.devtools && !getenv("WEBKIT_INSPECTOR_HTTP_SERVER")) {
    g_inspector_port = free_loopback_port();
    if (g_inspector_port) setenv("WEBKIT_INSPECTOR_HTTP_SERVER", ("127.0.0.1:" + std::to_string(g_inspector_port)).c_str(), 1);
  }
  fld::mkdir_p(data_dir());
  g_bookmarks = load_bookmarks(bookmarks_path());
  g_history = load_history(history_path());
  if (urls.empty() && g_cfg.restore && !priv) {
    std::string s;
    if (fld::read_file(session_path(), s))
      for (auto &l : fld::split(s, '\n'))
        if (!fld::trim(l).empty()) urls.push_back(fld::trim(l));
  }

  // Set up GIO's session bus, network monitor and proxy resolver now, before
  // any WebKit type exists. WebKit opens a socket in a class_init (holding
  // GLib's class-init lock); the proxy resolver (libproxy) then asks for the
  // network monitor, which waits for the session bus, which dconf's worker
  // thread (started by GTK under libdecor) is creating and cannot finish
  // without that lock: fl-web hung before showing a window.
  if (GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr)) g_object_unref(bus);
  g_network_monitor_get_default();
  g_proxy_resolver_get_default();

  // Under 4 GB of RAM: WebKit frees memory early (its processes are asked to
  // stay within a quarter of the RAM) and keeps no page cache, before any
  // WebKit object exists
  unsigned long long ram = fld::total_ram_bytes();
  if (ram && ram < (4ull << 30)) {
    guint limit_mb = (guint)std::max(256ull, ram / 4 / (1024 * 1024));
    WebKitMemoryPressureSettings *mp = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(mp, limit_mb);
    webkit_memory_pressure_settings_set_conservative_threshold(mp, 0.33);
    webkit_network_session_set_memory_pressure_settings(mp);
    GObjectClass *cls = G_OBJECT_CLASS(g_type_class_ref(WEBKIT_TYPE_WEB_CONTEXT));
    if (g_object_class_find_property(cls, "memory-pressure-settings"))
      web::g_web_context = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", mp, nullptr));
    g_type_class_unref(cls);
    webkit_memory_pressure_settings_free(mp);
    webkit_web_context_set_cache_model(web::g_web_context ? web::g_web_context : webkit_web_context_get_default(),
                                       WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER);
  }

  std::string cache = fld::cache_home() + "/fl-web";
  g_session =webkit_network_session_new(data_dir().c_str(), cache.c_str());
  webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(g_session),
                                               (data_dir() + "/cookies.sqlite").c_str(),
                                               WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  g_signal_connect(g_session, "download-started", G_CALLBACK(download_started), nullptr);
  g_settings = webkit_settings_new();
  webkit_settings_set_user_agent_with_application_details(g_settings, "fl-web", FLD_VERSION);
  webkit_settings_set_enable_developer_extras(g_settings, g_inspector_port != 0);
  {
    // WebKit says which parts of a frame changed, so only those are copied
    // and drawn (a blinking caret no longer costs a whole frame)
    WebKitFeatureList *all = webkit_settings_get_all_features();
    for (gsize i = 0; i < webkit_feature_list_get_length(all); i++) {
      WebKitFeature *f = webkit_feature_list_get(all, i);
      if (!strcmp(webkit_feature_get_identifier(f), "PropagateDamagingInformation"))
        webkit_settings_set_feature_enabled(g_settings, f, TRUE);
    }
    webkit_feature_list_unref(all);
  }
  apply_settings();
  g_ucm = webkit_user_content_manager_new();
  web::adblock::init(g_ucm, data_dir(), cache + "/adblock");
#ifdef FLD_HAVE_SECRET
  // logins sent by pages: offered for the keyring
  if (webkit_user_content_manager_register_script_message_handler(g_ucm, "flwebLogin", nullptr)) {
    g_signal_connect(g_ucm, "script-message-received::flwebLogin", CB([](WebKitUserContentManager *, JSCValue *v, gpointer) {
      char *s = jsc_value_to_string(v);
      std::string json = s ? s : "";
      g_free(s);
      later([json] { login_submitted(json); });
    }), nullptr);
    WebKitUserScript *us = webkit_user_script_new(login_capture_script(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                  WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, nullptr, nullptr);
    webkit_user_content_manager_add_script(g_ucm, us);
    webkit_user_script_unref(us);
  }
#endif
  // userscripts: data_dir()/userscripts/*.user.js, on the pages their @match names
  for (auto &n : fld::list_dir(data_dir() + "/userscripts")) {
    if (!fld::ends_with(n, ".js")) continue;
    std::string text;
    web::UserScript us;
    if (!fld::read_file(data_dir() + "/userscripts/" + n, text, 4u << 20) || !web::parse_userscript(text, us)) continue;
    std::vector<const char *> allow, block;
    for (auto &p : us.matches) allow.push_back(p.c_str());
    for (auto &p : us.excludes) block.push_back(p.c_str());
    allow.push_back(nullptr);
    block.push_back(nullptr);
    WebKitUserScript *s = webkit_user_script_new(us.source.c_str(), WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                 us.at_start ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START
                                                             : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END,
                                                 allow.data(), us.excludes.empty() ? nullptr : block.data());
    webkit_user_content_manager_add_script(g_ucm, s);
    webkit_user_script_unref(s);
  }
  Backend::on_copy = [](const std::string &text) { Fl::copy(text.c_str(), (int)text.size(), 1); };

  glib_loop_start();
  web::adblock::apply(g_cfg.adblock());
  new_window(urls, priv);
  int r = Fl::run();
  save_session();
  return r;
}
