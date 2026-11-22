// Tab state files for moving tabs between windows (see fld/tab_move.h)
#include "fld/tab_move.h"

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include "fld/json.h"
#include "fld/util.h"

namespace fld {

const char *const kTabPayloadPrefix = "x-fld-tab:";

std::string tab_dir() {
  std::string base = getenv_or("XDG_RUNTIME_DIR", "");
  if (base.empty() || !is_dir(base)) base = "/tmp/fldesktop-" + std::to_string(getuid());
  mkdir(base.c_str(), 0700);
  std::string d = base + "/fldesktop";
  mkdir(d.c_str(), 0700);
  d += "/tabs";
  mkdir(d.c_str(), 0700);
  return d;
}

static void remove_old(const std::string &dir) {
  time_t now = time(nullptr);
  for (auto &n : list_dir(dir, true)) {
    std::string p = dir + "/" + n;
    struct stat st;
    if (lstat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && now - st.st_mtime > 60) unlink(p.c_str());
  }
}

static bool write_private(const std::string &path, const std::string &data) {
  std::string tmp = path + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wx");
  if (!f) return false;
  fchmod(fileno(f), 0600);
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = fclose(f) == 0 && ok;
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

static std::string encode(const std::string &app, const std::string &state, const std::string &label, int pid) {
  Json j = Json::object();
  j.set("app", app);
  j.set("pid", pid);
  j.set("label", label);
  j.set("state", state);
  return j.dump();
}

std::string write_tab_file(const std::string &app, const std::string &state, const std::string &label) {
  std::string dir = tab_dir();
  remove_old(dir);
  static int seq = 0;
  std::string file = dir + "/tab-" + std::to_string(getpid()) + "-" + std::to_string(++seq) + "-" +
                     std::to_string((long)time(nullptr)) + ".json";
  if (!write_private(file, encode(app, state, label, (int)getpid()))) return "";
  return file;
}

bool parse_tab_payload(const std::string &text, std::string &file) {
  std::string p = kTabPayloadPrefix;
  if (text.compare(0, p.size(), p) != 0) return false;
  std::string f = trim(text.substr(p.size()));
  std::string dir = tab_dir() + "/";
  if (f.compare(0, dir.size(), dir) != 0) return false;
  std::string name = f.substr(dir.size());
  if (name.empty() || name.find('/') != std::string::npos || name[0] == '.') return false;
  file = f;
  return true;
}

bool claim_tab_file(const std::string &file, const std::string &app, std::string &state, int *pid) {
  std::string text;
  if (!read_file(file, text, 64u << 20)) return false;
  Json j;
  if (!Json::parse(text, j) || j["app"].as_string() != app) return false;
  // the rename is the claim: a second claimer finds no file
  std::string claimed = file + ".claimed-" + std::to_string(getpid());
  if (rename(file.c_str(), claimed.c_str()) != 0) return false;
  if (!read_file(claimed, text, 64u << 20) || !Json::parse(text, j)) {
    unlink(claimed.c_str());
    return false;
  }
  unlink(claimed.c_str());
  state = j["state"].as_string();
  if (pid) *pid = (int)j["pid"].as_number();
  return true;
}

bool restore_tab_file(const std::string &file, const std::string &app, const std::string &state, int pid) {
  return write_private(file, encode(app, state, "", pid));
}

std::string self_program(const std::string &fallback) {
  char buf[4096];
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n <= 0) return fallback;
  buf[n] = 0;
  std::string p = buf;
  // " (deleted)": the program was replaced by an update; the new one is in PATH
  if (ends_with(p, " (deleted)") || access(p.c_str(), X_OK) != 0) return fallback;
  return p;
}

void discard_tab_file(const std::string &file) {
  std::string f;
  if (parse_tab_payload(kTabPayloadPrefix + file, f)) unlink(f.c_str());
}

} // namespace fld
