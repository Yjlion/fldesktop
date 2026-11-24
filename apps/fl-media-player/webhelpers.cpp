#include "webhelpers.h"

#include <FL/Fl.H>
#include <atomic>
#include <ctime>
#include <glib.h>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

#include "fld/ini.h"
#include "fld/util.h"
#include "webhelpers_util.h"

namespace webhelpers {

namespace {
std::atomic<bool> g_updating{false};
std::mutex g_mu;
std::map<std::string, std::string> g_versions; // yt-dlp path -> --version

std::string state_path() { return fld::fld_config_dir() + "/media-player.ini"; }
bool executable(const std::string &p) { return !p.empty() && access(p.c_str(), X_OK) == 0; }
std::string managed_ytdlp() { return bin_dir() + "/yt-dlp"; }
std::string managed_qjs() { return bin_dir() + "/qjs"; }

std::string machine() {
  struct utsname u;
  return uname(&u) == 0 ? u.machine : "";
}

std::string sha256_of(const std::string &data) {
  gchar *s = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)data.data(), data.size());
  std::string r = s ? s : "";
  g_free(s);
  return r;
}

std::string sha256_of_file(const std::string &path) {
  std::string data;
  if (!fld::read_file(path, data, 256u << 20)) return "";
  return sha256_of(data);
}

// curl: everything a desktop has, and it follows GitHub's redirects
bool fetch(const std::string &url, std::string &out, std::string &err, bool api = false) {
  if (fld::which("curl").empty()) {
    err = "curl is not installed";
    return false;
  }
  std::vector<std::string> cmd = {"curl", "-fsSL", "--max-time", "300", "--retry", "2"};
  if (api) {
    cmd.push_back("-H");
    cmd.push_back("Accept: application/vnd.github+json");
  }
  cmd.push_back(url);
  int rc = fld::run_capture(cmd, out);
  if (rc != 0) {
    err = "the download from " + url + " failed (curl exit status " + std::to_string(rc) + ")";
    return false;
  }
  return true;
}

// verified, then swapped in at once (a running yt-dlp keeps its old file)
std::string install(const std::string &data, const std::string &path, const std::string &sum) {
  if (sha256_of(data) != sum) return "the download is damaged (wrong checksum), so it was not installed";
  fld::mkdir_p(bin_dir(), 0700);
  std::string tmp = path + ".new";
  if (!fld::write_file(tmp, data)) return "could not save it to " + tmp;
  if (chmod(tmp.c_str(), 0755) != 0 || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return "could not install it to " + path;
  }
  return "";
}

std::string version_of(const std::string &ytdlp) {
  {
    std::lock_guard<std::mutex> l(g_mu);
    auto it = g_versions.find(ytdlp);
    if (it != g_versions.end()) return it->second;
  }
  std::string out;
  std::string v;
  if (fld::run_capture({ytdlp, "--ignore-config", "--version"}, out) == 0) v = fld::trim(fld::split(out, '\n')[0]);
  std::lock_guard<std::mutex> l(g_mu);
  g_versions[ytdlp] = v;
  return v;
}
} // namespace

std::string bin_dir() { return fld::data_home() + "/fldesktop/media-player/bin"; }

State load_state() {
  fld::Ini ini;
  ini.load(state_path());
  State s;
  s.manage = ini.get_bool("Helpers", "Manage", true);
  s.checked = atoll(ini.get("Helpers", "Checked", "0").c_str());
  s.ytdlp_version = ini.get("Helpers", "YtdlpVersion");
  s.qjs_version = ini.get("Helpers", "QjsVersion");
  return s;
}

void save_state(const State &s) {
  fld::Ini ini;
  ini.load(state_path());
  ini.set_bool("Helpers", "Manage", s.manage);
  ini.set("Helpers", "Checked", std::to_string(s.checked));
  ini.set("Helpers", "YtdlpVersion", s.ytdlp_version);
  ini.set("Helpers", "QjsVersion", s.qjs_version);
  fld::mkdir_p(fld::fld_config_dir());
  ini.save(state_path());
}

std::string ytdlp_path() {
  if (load_state().manage && executable(managed_ytdlp())) return managed_ytdlp();
  return fld::which("yt-dlp");
}

std::vector<std::string> resolve_command(const std::string &page_url) {
  std::string y = ytdlp_path();
  bool ours = load_state().manage;
  std::string qjs = ours && executable(managed_qjs()) ? managed_qjs() : "";
  std::string js = webhelpers::js_runtime(!fld::which("deno").empty(), qjs, !fld::which("qjs").empty(),
                                          y.empty() ? "" : version_of(y));
  return resolve_cmd(y.empty() ? "yt-dlp" : y, js, page_url);
}

bool update_due() {
  State s = load_state();
  if (!s.manage) return false;
  if (!executable(managed_ytdlp())) return true;
  if (!qjs_asset(machine()).empty() && !executable(managed_qjs())) return true;
  return (long long)time(nullptr) - s.checked > 24 * 3600;
}

bool updating() { return g_updating.load(); }

void update(std::function<void(bool, bool, const std::string &)> done) {
  if (g_updating.exchange(true)) return;
  std::thread([done] {
    State s = load_state();
    std::vector<std::string> errors;
    bool changed = false;
    std::string err;

    // yt-dlp: the zipimport build runs with the system's Python 3
    if (fld::which("python3").empty()) {
      errors.push_back("yt-dlp needs Python 3, which is not installed");
    } else {
      std::string sums;
      if (!fetch(std::string(kYtdlpBase) + "SHA2-256SUMS", sums, err)) {
        errors.push_back("could not check for a new yt-dlp: " + err);
      } else {
        std::string sum = sha256_in_sums(sums, "yt-dlp");
        if (sum.empty()) {
          errors.push_back("could not check for a new yt-dlp: the checksum list has an unexpected format");
        } else if (sha256_of_file(managed_ytdlp()) != sum) {
          std::string data;
          if (!fetch(std::string(kYtdlpBase) + "yt-dlp", data, err)) errors.push_back("yt-dlp: " + err);
          else if (!(err = install(data, managed_ytdlp(), sum)).empty()) errors.push_back("yt-dlp: " + err);
          else changed = true;
        }
      }
    }

    // QuickJS-ng, for YouTube's JavaScript challenges
    std::string asset = qjs_asset(machine());
    if (!asset.empty()) {
      std::string json, tag, sum, url;
      if (!fetch(kQjsApi, json, err, true)) {
        errors.push_back("could not check for a new QuickJS-ng: " + err);
      } else if (!qjs_release_asset(json, asset, tag, sum, url)) {
        errors.push_back("could not check for a new QuickJS-ng: the release information has an unexpected format");
      } else if (sha256_of_file(managed_qjs()) == sum) {
        s.qjs_version = tag;
      } else {
        std::string data;
        if (!fetch(url, data, err)) errors.push_back("QuickJS-ng: " + err);
        else if (!(err = install(data, managed_qjs(), sum)).empty()) errors.push_back("QuickJS-ng: " + err);
        else {
          s.qjs_version = tag;
          changed = true;
        }
      }
    }

    // run ours once: it proves it works (Python 3.10 or newer) and gives the version
    if (executable(managed_ytdlp())) {
      {
        std::lock_guard<std::mutex> l(g_mu);
        g_versions.erase(managed_ytdlp());
      }
      std::string v = version_of(managed_ytdlp());
      if (v.empty()) errors.push_back("the downloaded yt-dlp does not run: is Python 3.10 or newer installed?");
      else s.ytdlp_version = v;
    }
    if (errors.empty()) s.checked = (long long)time(nullptr);
    std::string msg = errors.empty()
                          ? "yt-dlp " + (s.ytdlp_version.empty() ? "?" : s.ytdlp_version) +
                                (s.qjs_version.empty() ? "" : ", QuickJS-ng " + s.qjs_version)
                          : fld::join(errors, "; ");
    struct R {
      std::function<void(bool, bool, const std::string &)> done;
      State s;
      bool ok, changed;
      std::string msg;
    };
    R *r = new R{done, s, errors.empty(), changed, msg};
    Fl::awake([](void *d) {
      R *r = (R *)d;
      // saved here: the settings file is only written from the FLTK thread
      State cur = load_state();
      cur.checked = r->s.checked ? r->s.checked : cur.checked;
      cur.ytdlp_version = r->s.ytdlp_version;
      cur.qjs_version = r->s.qjs_version;
      save_state(cur);
      g_updating = false;
      if (r->done) r->done(r->ok, r->changed, r->msg);
      delete r;
    }, r);
  }).detach();
}

} // namespace webhelpers
