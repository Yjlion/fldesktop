// fl-web: ad and tracker blocking with WebKit content blockers
#include "adblock.h"

#include <FL/Fl.H>
#include <wpe/webkit.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <ctime>
#include <thread>

#include "adblock_rules.h"
#include "fld/ini.h"
#include "fld/util.h"
#include "glib_loop.h"

namespace web::adblock {

std::function<void()> on_status;

namespace {

const char *kId = "fl-adblock";
const char *kEasyList = "https://easylist.to/easylist/easylist.txt";
const char *kEasyPrivacy = "https://easylist.to/easylist/easyprivacy.txt";
const time_t kMaxAge = 4 * 24 * 3600;

struct State {
  WebKitUserContentManager *ucm = nullptr;
  WebKitUserContentFilterStore *store = nullptr;
  WebKitNetworkSession *session = nullptr; // for the lists: no cookies, not in Downloads
  std::string cache_dir;
  Settings settings;
  bool active = false;       // a filter is in the content manager
  std::string active_key;    // the settings it was built for
  std::string building_key;  // being converted or compiled
  bool rebuild_again = false;
  int downloading = 0;
  std::string status;
  std::vector<std::function<void()>> waiting; // apply()'s done callbacks
} g;

std::string state_path() { return g.cache_dir + "/state.ini"; }

void set_status(const std::string &s) {
  g.status = s;
  if (on_status) on_status();
}

std::vector<std::string> urls(const Settings &s) {
  std::vector<std::string> v;
  if (s.easylist) v.push_back(kEasyList);
  if (s.easyprivacy) v.push_back(kEasyPrivacy);
  for (auto &u : s.extra)
    if (!fld::trim(u).empty()) v.push_back(fld::trim(u));
  return v;
}

std::string list_path(const std::string &url) {
  char buf[32];
  snprintf(buf, sizeof buf, "list-%016llx.txt", (unsigned long long)abp_fnv1a(url));
  return g.cache_dir + "/" + buf;
}

// the settings a compiled filter was made for; one line, so state.ini can keep it
std::string key(const Settings &s) {
  std::vector<std::string> f{"lists"};
  for (auto &u : urls(s)) f.push_back(u);
  f.push_back("allow");
  for (auto &a : s.allow) f.push_back(a);
  return abp_key(f);
}

// the downloaded lists as they are now: names the converted rules without
// the allowlist, which are kept so that changing it only recompiles
std::string base_key(const Settings &s) {
  std::vector<std::string> f;
  for (auto &u : urls(s)) {
    struct stat st;
    std::string p = list_path(u);
    bool ok = stat(p.c_str(), &st) == 0;
    f.push_back(u);
    f.push_back(ok ? std::to_string((long long)st.st_mtime) + " " + std::to_string((long long)st.st_size) : "-");
  }
  return abp_key(f);
}

std::string base_path(const std::string &dir, const std::string &bkey, bool cosmetic) {
  return dir + "/base-" + bkey + (cosmetic ? "-c1" : "-c0") + ".json";
}

const char *kBaseMagic = "fl-adblock-base";

time_t mtime(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
}

void finish_waiting() {
  auto w = std::move(g.waiting);
  g.waiting.clear();
  for (auto &f : w)
    if (f) f();
}

void describe(size_t rules) {
  time_t newest = 0;
  for (auto &u : urls(g.settings)) newest = std::max(newest, mtime(list_path(u)));
  char date[64] = "";
  if (newest) strftime(date, sizeof date, "%e %b %Y", localtime(&newest));
  std::string n = std::to_string(rules);
  for (int i = (int)n.size() - 3; i > 0; i -= 3) n.insert((size_t)i, " ");
  set_status(std::string("Lists updated ") + fld::trim(date) + ", " + n + " rules");
}

void use_filter(WebKitUserContentFilter *f, const std::string &k) {
  webkit_user_content_manager_remove_all_filters(g.ucm);
  if (g.settings.enabled && f) webkit_user_content_manager_add_filter(g.ucm, f);
  g.active = g.settings.enabled && f;
  g.active_key = k;
  glib_loop_poke();
}

void rebuild();

struct Job {
  std::vector<std::string> texts;
  std::vector<std::string> allow;
  std::string key;
  std::string dir, bkey; // where the converted base rules are kept
  bool cosmetic = true;
  AdblockResult result;
};

bool read_base(const std::string &path, AdblockResult &r) {
  std::string t;
  if (!fld::read_file(path, t, 512u << 20)) return false;
  size_t nl = t.find('\n');
  unsigned long long rules = 0, skipped = 0;
  char magic[32] = "";
  if (nl == std::string::npos || sscanf(t.c_str(), "%31s %llu %llu", magic, &rules, &skipped) != 3 ||
      std::string(magic) != kBaseMagic || t.size() < nl + 3 || t[nl + 1] != '[' || t.back() != ']')
    return false;
  r.json = t.substr(nl + 1);
  r.rules = (size_t)rules;
  r.skipped = (size_t)skipped;
  return true;
}

void write_base(const Job *job, const AdblockResult &r) {
  std::string path = base_path(job->dir, job->bkey, job->cosmetic);
  std::string head = std::string(kBaseMagic) + " " + std::to_string(r.rules) + " " + std::to_string(r.skipped) + "\n";
  if (!fld::write_file(path, head + r.json)) return;
  // only the lists as they are now are worth keeping
  std::string keep = "base-" + job->bkey + "-";
  for (auto &n : fld::list_dir(job->dir))
    if (n.rfind("base-", 0) == 0 && n.rfind(keep, 0) != 0) unlink((job->dir + "/" + n).c_str());
}

// on a worker thread: the rules for job, from the kept base rules when the
// lists have not changed
void prepare(Job *job) {
  AdblockResult r;
  if (job->cosmetic && read_base(base_path(job->dir, job->bkey, true), r)) {
  } else if (read_base(base_path(job->dir, job->bkey, false), r)) {
    job->cosmetic = false; // element hiding was refused before
  } else {
    r = abp_to_webkit(job->texts, {}, job->cosmetic);
    write_base(job, r);
  }
  abp_append_allow(r, job->allow);
  job->result = std::move(r);
}

void compile(Job *job);

void saved(GObject *store, GAsyncResult *res, gpointer data) {
  Job *job = (Job *)data;
  GError *err = nullptr;
  WebKitUserContentFilter *f =
      webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(store), res, &err);
  if (!f) {
    std::string msg = err ? err->message : "unknown error";
    if (err) g_error_free(err);
    if (job->cosmetic) {
      // an element hiding rule WebKit refuses: block without hiding
      fprintf(stderr, "fl-web: ad blocking rules not accepted (%s), retrying without element hiding\n", msg.c_str());
      unlink(base_path(job->dir, job->bkey, true).c_str());
      job->cosmetic = false;
      std::thread([job] {
        prepare(job);
        Fl::awake([](void *d) { compile((Job *)d); }, job);
      }).detach();
      return;
    }
    g.building_key.clear();
    set_status("The filter lists could not be used: " + msg);
    delete job;
    finish_waiting();
    return;
  }
  if (job->key == key(g.settings)) {
    use_filter(f, job->key);
    fld::Ini st;
    st.load(state_path());
    st.set("Adblock", "Key", job->key);
    st.set("Adblock", "Rules", std::to_string(job->result.rules));
    st.save(state_path());
    describe(job->result.rules);
  }
  webkit_user_content_filter_unref(f);
  g.building_key.clear();
  delete job;
  if (g.rebuild_again) {
    g.rebuild_again = false;
    rebuild();
  } else {
    finish_waiting();
  }
}

void compile(Job *job) {
  GBytes *b = g_bytes_new(job->result.json.data(), job->result.json.size());
  webkit_user_content_filter_store_save(g.store, kId, b, nullptr, saved, job);
  g_bytes_unref(b);
  glib_loop_poke();
}

// converts the downloaded lists and compiles them
void rebuild() {
  std::string k = key(g.settings);
  if (!g.building_key.empty()) {
    if (g.building_key != k) g.rebuild_again = true;
    return;
  }
  auto *job = new Job;
  for (auto &u : urls(g.settings)) {
    std::string text;
    if (fld::read_file(list_path(u), text)) job->texts.push_back(std::move(text));
  }
  if (job->texts.empty()) {
    delete job;
    use_filter(nullptr, k);
    if (!g.downloading) set_status(urls(g.settings).empty() ? "No filter lists chosen" : "The filter lists are not downloaded yet");
    finish_waiting();
    return;
  }
  job->allow = g.settings.allow;
  job->key = k;
  job->dir = g.cache_dir;
  job->bkey = base_key(g.settings);
  g.building_key = k;
  // the filter in use stays until the new one is ready
  bool kept = fld::file_exists(base_path(job->dir, job->bkey, true)) ||
              fld::file_exists(base_path(job->dir, job->bkey, false));
  set_status(kept ? "Updating the filter…" : "Preparing the filter lists…");
  std::thread([job] {
    prepare(job);
    Fl::awake([](void *d) { compile((Job *)d); }, job);
  }).detach();
}

struct Fetch {
  std::string url, path, tmp;
  bool ok = false, failed = false;
};

void fetch_done(Fetch *f) {
  if (f->ok) rename(f->tmp.c_str(), f->path.c_str());
  else unlink(f->tmp.c_str());
  delete f;
  if (--g.downloading == 0) rebuild();
}

void fetch(const std::string &url) {
  if (!g.session) g.session = webkit_network_session_new_ephemeral();
  auto *f = new Fetch;
  f->url = url;
  f->path = list_path(url);
  f->tmp = f->path + ".part";
  g.downloading++;
  set_status("Downloading the filter lists…");
  WebKitDownload *dl = webkit_network_session_download_uri(g.session, url.c_str());
  webkit_download_set_allow_overwrite(dl, TRUE);
  g_signal_connect(dl, "decide-destination", G_CALLBACK(+[](WebKitDownload *dl, char *, gpointer d) -> gboolean {
    webkit_download_set_destination(dl, ((Fetch *)d)->tmp.c_str());
    return TRUE;
  }), f);
  g_signal_connect(dl, "failed", G_CALLBACK(+[](WebKitDownload *, GError *e, gpointer d) {
    Fetch *f = (Fetch *)d;
    fprintf(stderr, "fl-web: could not download %s: %s\n", f->url.c_str(), e ? e->message : "?");
    f->failed = true;
  }), f);
  g_signal_connect(dl, "finished", G_CALLBACK(+[](WebKitDownload *dl, gpointer d) {
    Fetch *f = (Fetch *)d;
    // "finished" follows "failed" too; a list starts with "[Adblock" or "!"
    WebKitURIResponse *r = webkit_download_get_response(dl);
    std::string head;
    if (!f->failed && r && webkit_uri_response_get_status_code(r) == 200 && fld::read_file(f->tmp, head, 64))
      f->ok = head.rfind("[Adblock", 0) == 0 || head.rfind("!", 0) == 0;
    g_object_unref(dl);
    fetch_done(f);
  }), f);
  glib_loop_poke();
}

void download(bool all) {
  if (g.downloading) return;
  time_t now = time(nullptr);
  for (auto &u : urls(g.settings)) {
    time_t m = mtime(list_path(u));
    if (all || !m || now - m > kMaxAge) fetch(u);
  }
}

} // namespace

void init(WebKitUserContentManager *ucm, const std::string &data_dir, const std::string &cache_dir) {
  g.ucm = ucm;
  g.cache_dir = cache_dir;
  fld::mkdir_p(cache_dir);
  std::string store = data_dir + "/content-filters";
  fld::mkdir_p(store);
  g.store = webkit_user_content_filter_store_new(store.c_str());
}

void apply(const Settings &s, std::function<void()> done) {
  g.settings = s;
  if (!g.store) {
    if (done) done();
    return;
  }
  if (!s.enabled) {
    use_filter(nullptr, "");
    set_status("Off");
    if (done) done();
    return;
  }
  if (done) g.waiting.push_back(std::move(done));
  std::string k = key(s);
  fld::Ini st;
  st.load(state_path());
  if (g.active && g.active_key == k) {
    finish_waiting();
  } else if (!g.active && st.get("Adblock", "Key", "") == k) {
    // compiled on an earlier run: load it, no conversion
    size_t rules = (size_t)atol(st.get("Adblock", "Rules", "0").c_str());
    struct Load { std::string key; size_t rules; };
    webkit_user_content_filter_store_load(g.store, kId, nullptr, +[](GObject *store, GAsyncResult *res, gpointer d) {
      Load *l = (Load *)d;
      WebKitUserContentFilter *f =
          webkit_user_content_filter_store_load_finish(WEBKIT_USER_CONTENT_FILTER_STORE(store), res, nullptr);
      if (f && l->key == key(g.settings)) {
        use_filter(f, l->key);
        describe(l->rules);
        finish_waiting();
      } else {
        rebuild();
      }
      if (f) webkit_user_content_filter_unref(f);
      delete l;
    }, new Load{k, rules});
    glib_loop_poke();
  } else {
    rebuild();
  }
  download(false);
}

void update_now() {
  if (g.store && g.settings.enabled) download(true);
}

std::string status() { return g.status; }

std::string convert_files(const std::vector<std::string> &paths, size_t *rules, size_t *skipped) {
  std::vector<std::string> texts;
  for (auto &p : paths) {
    std::string t;
    if (fld::read_file(p, t)) texts.push_back(std::move(t));
  }
  AdblockResult r = abp_to_webkit(texts, {});
  if (rules) *rules = r.rules;
  if (skipped) *skipped = r.skipped;
  return r.json;
}

} // namespace web::adblock
