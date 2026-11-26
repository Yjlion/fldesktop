#include "fld/util.h"
#include "fld/ini.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fld {

std::string getenv_or(const char *name, const std::string &fallback) {
  const char *v = getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

std::string home_dir() {
  const char *h = getenv("HOME");
  if (h && *h) return h;
  struct passwd *pw = getpwuid(getuid());
  return pw ? pw->pw_dir : "/";
}

std::string config_home() { return getenv_or("XDG_CONFIG_HOME", home_dir() + "/.config"); }
std::string data_home()   { return getenv_or("XDG_DATA_HOME", home_dir() + "/.local/share"); }
std::string cache_home()  { return getenv_or("XDG_CACHE_HOME", home_dir() + "/.cache"); }

std::vector<std::string> data_dirs() {
  std::vector<std::string> v{data_home()};
  for (auto &d : split(getenv_or("XDG_DATA_DIRS", "/usr/local/share:/usr/share"), ':'))
    if (std::find(v.begin(), v.end(), d) == v.end()) v.push_back(d);
  return v;
}

std::vector<std::string> config_dirs() {
  std::vector<std::string> v{config_home()};
  for (auto &d : split(getenv_or("XDG_CONFIG_DIRS", "/etc/xdg"), ':')) v.push_back(d);
  return v;
}

std::string fld_config_dir() {
  std::string d = config_home() + "/fldesktop";
  mkdir_p(d, 0700);
  return d;
}

std::string user_dir(const char *key) {
  // ~/.config/user-dirs.dirs: XDG_DESKTOP_DIR="$HOME/Desktop"
  std::string data, want = std::string("XDG_") + key + "_DIR=";
  if (read_file(config_home() + "/user-dirs.dirs", data)) {
    for (auto &line : split(data, '\n')) {
      std::string l = trim(line);
      if (!starts_with(l, want)) continue;
      std::string v = l.substr(want.size());
      if (v.size() >= 2 && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size() - 2);
      if (starts_with(v, "$HOME")) v = home_dir() + v.substr(5);
      return v;
    }
  }
  std::string k = lower(key);
  if (k == "desktop") return home_dir() + "/Desktop";
  if (k == "download") return home_dir() + "/Downloads";
  if (k == "documents") return home_dir() + "/Documents";
  if (k == "pictures") return home_dir() + "/Pictures";
  if (k == "music") return home_dir() + "/Music";
  if (k == "videos") return home_dir() + "/Videos";
  return home_dir();
}

std::string fld_data_dir() { return getenv_or("FLD_DATADIR", FLD_DATADIR); }

bool file_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool is_dir(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool mkdir_p(const std::string &path, mode_t mode) {
  if (path.empty()) return false;
  if (is_dir(path)) return true;
  std::string parent = dirname(path);
  if (parent != path && !is_dir(parent)) mkdir_p(parent, mode);
  return mkdir(path.c_str(), mode) == 0 || errno == EEXIST;
}

bool read_file(const std::string &path, std::string &out, size_t max) {
  FILE *f = fopen(path.c_str(), "rb");
  if (!f) return false;
  out.clear();
  char buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
    out.append(buf, n);
    if (out.size() > max) break;
  }
  bool ok = !ferror(f);
  fclose(f);
  return ok;
}

bool write_file(const std::string &path, const std::string &data) {
  std::string tmp = path + ".tmp~";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f) return false;
  bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
  ok = (fflush(f) == 0) && ok;
  fclose(f);
  if (!ok) { unlink(tmp.c_str()); return false; }
  // keep permissions of an existing file
  struct stat st;
  if (stat(path.c_str(), &st) == 0) chmod(tmp.c_str(), st.st_mode & 07777);
  return rename(tmp.c_str(), path.c_str()) == 0;
}

std::string path_join(const std::string &a, const std::string &b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  if (b[0] == '/') return b;
  if (a.back() == '/') return a + b;
  return a + "/" + b;
}

std::string basename(const std::string &path) {
  if (path == "/") return "/";
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

std::string dirname(const std::string &path) {
  std::string p = path;
  while (p.size() > 1 && p.back() == '/') p.pop_back();
  size_t s = p.rfind('/');
  if (s == std::string::npos) return ".";
  if (s == 0) return "/";
  return p.substr(0, s);
}

int natural_compare(const std::string &a, const std::string &b) {
  size_t i = 0, j = 0, na = a.size(), nb = b.size();
  int zeros = 0; // tie-break: fewer leading zeros first ("a1" < "a01")
  while (i < na && j < nb) {
    unsigned char ca = (unsigned char)a[i], cb = (unsigned char)b[j];
    if (isdigit(ca) && isdigit(cb)) {
      size_t zi = i, zj = j;
      while (zi < na && a[zi] == '0') zi++;
      while (zj < nb && b[zj] == '0') zj++;
      size_t ei = zi, ej = zj;
      while (ei < na && isdigit((unsigned char)a[ei])) ei++;
      while (ej < nb && isdigit((unsigned char)b[ej])) ej++;
      // the value: more significant digits is larger, then digit by digit
      if (ei - zi != ej - zj) return ei - zi < ej - zj ? -1 : 1;
      for (size_t k = 0; k < ei - zi; k++)
        if (a[zi + k] != b[zj + k]) return a[zi + k] < b[zj + k] ? -1 : 1;
      if (!zeros && (zi - i) != (zj - j)) zeros = (zi - i) < (zj - j) ? -1 : 1;
      i = ei;
      j = ej;
      continue;
    }
    int la = tolower(ca), lb = tolower(cb);
    if (la != lb) return la < lb ? -1 : 1;
    i++;
    j++;
  }
  if (i < na || j < nb) return i < na ? 1 : -1;
  if (zeros) return zeros;
  return a.compare(b) < 0 ? -1 : a.compare(b) > 0 ? 1 : 0;
}

std::string extension(const std::string &path) {
  std::string b = basename(path);
  size_t d = b.rfind('.');
  if (d == std::string::npos || d == 0) return "";
  return lower(b.substr(d + 1));
}

std::string abs_path(const std::string &path) {
  std::string p = expand_tilde(path);
  if (p.empty()) return p;
  if (p[0] != '/') {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof cwd)) p = path_join(cwd, p);
  }
  // normalise . and ..
  std::vector<std::string> out;
  for (auto &c : split(p, '/')) {
    if (c.empty() || c == ".") continue;
    if (c == "..") { if (!out.empty()) out.pop_back(); continue; }
    out.push_back(c);
  }
  return "/" + join(out, "/");
}

std::string tilde(const std::string &path) {
  std::string h = home_dir();
  if (path == h) return "~";
  if (starts_with(path, h + "/")) return "~" + path.substr(h.size());
  return path;
}

std::string expand_tilde(const std::string &path) {
  if (path == "~") return home_dir();
  if (starts_with(path, "~/")) return home_dir() + path.substr(1);
  return path;
}

std::string unique_name(const std::string &dir, const std::string &name) {
  std::string p = path_join(dir, name);
  if (!file_exists(p)) return p;
  std::string stem = name, ext;
  size_t d = name.rfind('.');
  if (d != std::string::npos && d > 0) { stem = name.substr(0, d); ext = name.substr(d); }
  for (int i = 2; i < 10000; i++) {
    p = path_join(dir, stem + " (" + std::to_string(i) + ")" + ext);
    if (!file_exists(p)) return p;
  }
  return p;
}

std::vector<std::string> list_dir(const std::string &dir, bool hidden) {
  std::vector<std::string> v;
  DIR *d = opendir(dir.c_str());
  if (!d) return v;
  while (struct dirent *e = readdir(d)) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    if (!hidden && e->d_name[0] == '.') continue;
    v.push_back(e->d_name);
  }
  closedir(d);
  std::sort(v.begin(), v.end());
  return v;
}

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

bool starts_with(const std::string &s, const std::string &p) {
  return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string &s, const std::string &p) {
  return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

std::vector<std::string> split(const std::string &s, char sep, bool keep_empty) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      if (keep_empty || !cur.empty()) v.push_back(cur);
      cur.clear();
    } else cur += c;
  }
  if (keep_empty || !cur.empty()) v.push_back(cur);
  return v;
}

std::string join(const std::vector<std::string> &v, const std::string &sep) {
  std::string r;
  for (size_t i = 0; i < v.size(); i++) {
    if (i) r += sep;
    r += v[i];
  }
  return r;
}

std::string shell_quote(const std::string &s) {
  std::string r = "'";
  for (char c : s) {
    if (c == '\'') r += "'\\''";
    else r += c;
  }
  return r + "'";
}

std::vector<std::string> shell_split(const std::string &cmd) {
  std::vector<std::string> v;
  std::string cur;
  bool have = false;
  char q = 0;
  for (size_t i = 0; i < cmd.size(); i++) {
    char c = cmd[i];
    if (q) {
      if (c == q) q = 0;
      else if (c == '\\' && q == '"' && i + 1 < cmd.size() &&
               strchr("\"\\$`", cmd[i + 1])) cur += cmd[++i];
      else cur += c;
    } else if (c == '\'' || c == '"') { q = c; have = true; }
    else if (c == '\\' && i + 1 < cmd.size()) { cur += cmd[++i]; have = true; }
    else if (c == ' ' || c == '\t' || c == '\n') {
      if (have) { v.push_back(cur); cur.clear(); have = false; }
    } else { cur += c; have = true; }
  }
  if (have) v.push_back(cur);
  return v;
}

std::string human_size(unsigned long long b) {
  const char *u[] = {"B", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double v = (double)b;
  int i = 0;
  while (v >= 1024 && i < 5) { v /= 1024; i++; }
  char buf[32];
  if (i == 0) snprintf(buf, sizeof buf, "%llu B", b);
  else snprintf(buf, sizeof buf, v < 10 ? "%.1f %s" : "%.0f %s", v, u[i]);
  return buf;
}

bool strftime_has_seconds(const std::string &fmt) {
  for (size_t i = 0; i + 1 < fmt.size(); i++) {
    if (fmt[i] != '%') continue;
    size_t j = i + 1;
    // glibc flags and width (%-S, %_3S), then the E / O modifiers
    while (j < fmt.size() && (strchr("_-0^#", fmt[j]) || isdigit((unsigned char)fmt[j]))) j++;
    if (j < fmt.size() && (fmt[j] == 'E' || fmt[j] == 'O')) j++;
    if (j >= fmt.size()) break;
    if (strchr("STrXcs+", fmt[j])) return true;
    i = j; // "%%" too: the second '%' is not a conversion
  }
  return false;
}

double strftime_next_change(const std::string &fmt, double now) {
  double frac = now - std::floor(now);
  if (strftime_has_seconds(fmt)) return 1.0 - frac;
  time_t t = (time_t)std::floor(now);
  struct tm tm;
  localtime_r(&t, &tm);
  return std::max(0.0, (60 - tm.tm_sec) - frac); // tm_sec is 60 in a leap second
}

std::string format_time(time_t t, const char *fmt) {
  char buf[64];
  struct tm tm;
  localtime_r(&t, &tm);
  strftime(buf, sizeof buf, fmt, &tm);
  return buf;
}

std::string uri_encode_path(const std::string &path) {
  static const char *hex = "0123456789ABCDEF";
  std::string r;
  for (unsigned char c : path) {
    if (isalnum(c) || strchr("/-_.~", c)) r += (char)c;
    else { r += '%'; r += hex[c >> 4]; r += hex[c & 15]; }
  }
  return r;
}

std::string uri_decode(const std::string &s) {
  std::string r;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
        isxdigit((unsigned char)s[i + 2])) {
      r += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else r += s[i];
  }
  return r;
}

static void close_fds_from(int lowfd) {
  long max = sysconf(_SC_OPEN_MAX);
  if (max < 0 || max > 65536) max = 65536;
  for (int fd = lowfd; fd < max; fd++) close(fd);
}

std::map<std::string, std::string> parse_env_text(const std::string &text) {
  std::map<std::string, std::string> m;
  for (auto &line : split(text, '\n')) {
    std::string l = trim(line);
    if (l.empty() || l[0] == '#') continue;
    if (starts_with(l, "export ")) l = trim(l.substr(7));
    size_t eq = l.find('=');
    if (eq == std::string::npos || eq == 0) continue;
    std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
    if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v.back() == v[0]) v = v.substr(1, v.size() - 2);
    m[k] = v;
  }
  return m;
}

std::string session_env_snapshot_path() {
  return getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fldesktop-session.env";
}

std::vector<std::pair<std::string, std::string>> session_env_changes() {
  // wm.ini [environment]: "KEY=value" sets, "KEY=" unsets, "KEY=@inherit"
  // restores what the session started with (fl-session's snapshot)
  std::vector<std::pair<std::string, std::string>> out;
  Ini wm;
  if (!wm.load(fld_config_dir() + "/wm.ini")) return out;
  std::string snap_text;
  bool have_snap = read_file(session_env_snapshot_path(), snap_text);
  auto snap = parse_env_text(snap_text);
  for (auto &k : wm.keys("environment")) {
    std::string v = wm.get("environment", k);
    if (v == "@inherit") {
      if (!have_snap) continue;  // no snapshot: keep what we have
      auto it = snap.find(k);
      out.push_back({k, it == snap.end() ? std::string(1, '\x01') : it->second});
    } else {
      out.push_back({k, v.empty() ? std::string(1, '\x01') : v});
    }
  }
  return out;
}

extern "C" char **environ;

bool process_running(const std::string &comm) {
  std::string want = comm.substr(0, 15);
  uid_t me = getuid();
  for (auto &pid : list_dir("/proc")) {
    if (pid.empty() || pid[0] < '0' || pid[0] > '9') continue;
    struct stat st;
    if (stat(("/proc/" + pid).c_str(), &st) != 0 || st.st_uid != me) continue;
    std::string c;
    if (read_file("/proc/" + pid + "/comm", c, 64) && trim(c) == want) return true;
  }
  return false;
}

bool ensure_running(const std::vector<std::string> &argv) {
  if (argv.empty()) return false;
  if (process_running(basename(argv[0]))) return true;
  return spawn(argv);
}

bool spawn(const std::vector<std::string> &argv, const std::string &cwd) {
  if (argv.empty()) return false;
  // the session environment as configured now (e.g. a proxy set in Settings
  // after this program started); prepared before fork
  std::vector<std::string> env_store;
  for (char **e = environ; e && *e; e++) env_store.push_back(*e);
  for (auto &kv : session_env_changes()) {
    std::string prefix = kv.first + "=";
    env_store.erase(std::remove_if(env_store.begin(), env_store.end(),
                                   [&](const std::string &x) { return starts_with(x, prefix); }),
                    env_store.end());
    if (kv.second != std::string(1, '\x01')) env_store.push_back(prefix + kv.second);
  }
  std::vector<char *> envp;
  for (auto &e : env_store) envp.push_back(const_cast<char *>(e.c_str()));
  envp.push_back(nullptr);
  int pipefd[2];
  if (pipe2(pipefd, O_CLOEXEC) < 0) return false;
  pid_t pid = fork();
  if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return false; }
  if (pid == 0) {
    setsid();
    pid_t p2 = fork();
    if (p2 != 0) _exit(0);
    close(pipefd[0]);
    // restore default signal handling
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    for (int s = 1; s < NSIG; s++) sigaction(s, &sa, nullptr);
    sigset_t set;
    sigemptyset(&set);
    sigprocmask(SIG_SETMASK, &set, nullptr);
    int devnull = open("/dev/null", O_RDONLY);
    if (devnull >= 0) { dup2(devnull, 0); close(devnull); }
    if (!cwd.empty() && chdir(cwd.c_str()) != 0) { /* ignore */ }
    else if (cwd.empty() && chdir(home_dir().c_str()) != 0) { /* ignore */ }
    std::vector<char *> args;
    for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    int keep = pipefd[1];
    // close everything above stderr except the status pipe
    long max = sysconf(_SC_OPEN_MAX);
    if (max < 0 || max > 65536) max = 65536;
    for (int fd = 3; fd < max; fd++) if (fd != keep) close(fd);
    execvpe(args[0], args.data(), envp.data());
    int e = errno;
    if (write(keep, &e, sizeof e) < 0) { /* nothing */ }
    _exit(127);
  }
  close(pipefd[1]);
  int status;
  waitpid(pid, &status, 0);
  int err = 0;
  ssize_t n = read(pipefd[0], &err, sizeof err);
  close(pipefd[0]);
  return n == 0; // exec succeeded (pipe closed by CLOEXEC)
}

bool spawn_shell(const std::string &cmdline, const std::string &cwd) {
  return spawn({"/bin/sh", "-c", cmdline}, cwd);
}

int run_capture(const std::vector<std::string> &argv, std::string &out) {
  if (argv.empty()) return -1;
  int p[2];
  if (pipe(p) < 0) return -1;
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    dup2(p[1], 1);
    close(p[0]);
    close(p[1]);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 2); close(devnull); }
    close_fds_from(3);
    std::vector<char *> args;
    for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(p[1]);
  out.clear();
  char buf[4096];
  ssize_t n;
  while ((n = read(p[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
    if (n > 0) out.append(buf, n);
  close(p[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int run_capture_input(const std::vector<std::string> &argv, const std::string &input, std::string &out,
                      bool merge_stderr) {
  if (argv.empty()) return -1;
  int po[2], pi[2];
  if (pipe(po) < 0) return -1;
  if (pipe(pi) < 0) {
    close(po[0]);
    close(po[1]);
    return -1;
  }
  pid_t pid = fork();
  if (pid < 0) {
    close(po[0]); close(po[1]); close(pi[0]); close(pi[1]);
    return -1;
  }
  if (pid == 0) {
    dup2(pi[0], 0);
    dup2(po[1], 1);
    if (merge_stderr) dup2(po[1], 2);
    else {
      int devnull = open("/dev/null", O_RDWR);
      if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
    }
    close(po[0]); close(po[1]); close(pi[0]); close(pi[1]);
    close_fds_from(3);
    std::vector<char *> args;
    for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(po[1]);
  close(pi[0]);
  // the input is small (a password line): write it all, then close
  void (*old)(int) = signal(SIGPIPE, SIG_IGN);
  size_t off = 0;
  while (off < input.size()) {
    ssize_t n = write(pi[1], input.data() + off, input.size() - off);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    off += (size_t)n;
  }
  close(pi[1]);
  signal(SIGPIPE, old);
  out.clear();
  char buf[4096];
  ssize_t n;
  while ((n = read(po[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR))
    if (n > 0) out.append(buf, n);
  close(po[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int run_lines(const std::vector<std::string> &argv, const std::function<void(const std::string &)> &on_line) {
  if (argv.empty()) return -1;
  int p[2];
  if (pipe(p) < 0) return -1;
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    return -1;
  }
  if (pid == 0) {
    dup2(p[1], 1);
    close(p[0]);
    close(p[1]);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) { dup2(devnull, 0); dup2(devnull, 2); close(devnull); }
    close_fds_from(3);
    std::vector<char *> args;
    for (auto &a : argv) args.push_back(const_cast<char *>(a.c_str()));
    args.push_back(nullptr);
    execvp(args[0], args.data());
    _exit(127);
  }
  close(p[1]);
  std::string pending;
  char buf[4096];
  ssize_t n;
  while ((n = read(p[0], buf, sizeof buf)) > 0 || (n < 0 && errno == EINTR)) {
    if (n <= 0) continue;
    pending.append(buf, n);
    size_t nl;
    while ((nl = pending.find('\n')) != std::string::npos) {
      std::string line = pending.substr(0, nl);
      pending.erase(0, nl + 1);
      if (on_line) on_line(line);
    }
  }
  if (!pending.empty() && on_line) on_line(pending);
  close(p[0]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

std::string which(const std::string &prog) {
  if (prog.find('/') != std::string::npos)
    return access(prog.c_str(), X_OK) == 0 ? prog : "";
  for (auto &d : split(getenv_or("PATH", "/usr/local/bin:/usr/bin:/bin"), ':')) {
    std::string p = path_join(d, prog);
    struct stat st;
    if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(p.c_str(), X_OK) == 0)
      return p;
  }
  return "";
}

std::string terminal_command() {
  Ini cfg;
  cfg.load(fld_config_dir() + "/desktop.ini");
  std::string t = cfg.get("General", "Terminal", "");
  if (!t.empty()) return t;
  for (const char *c : {"fl-term", "foot", "alacritty", "kitty", "xterm"})
    if (!which(c).empty()) return c;
  return "fl-term";
}

} // namespace fld

namespace fld {

bool reload_wm() {
  const char *pid = getenv("FLWM_PID");
  if (pid && atoi(pid) > 1) {
    // make sure the pid still belongs to fl-wm
    std::string comm;
    read_file(std::string("/proc/") + pid + "/comm", comm);
    if (trim(comm) == "fl-wm") return kill(atoi(pid), SIGHUP) == 0;
  }
  // started by D-Bus (the portal): our own fl-wm, found in /proc
  bool sent = false;
  for (const std::string &d : list_dir("/proc")) {
    if (d.find_first_not_of("0123456789") != std::string::npos) continue;
    struct stat st;
    if (stat(("/proc/" + d).c_str(), &st) != 0 || st.st_uid != getuid()) continue;
    std::string comm;
    read_file("/proc/" + d + "/comm", comm, 64);
    if (trim(comm) == "fl-wm" && kill(atoi(d.c_str()), SIGHUP) == 0) sent = true;
  }
  return sent;
}

std::string wm_config_path() { return fld_config_dir() + "/wm.ini"; }

void restart_self(int argc, char **argv) {
  (void)argc;
  char exe[PATH_MAX];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return;
  exe[n] = 0;
  execv(exe, argv);
}

unsigned long long total_ram_bytes() {
  long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
  return pages > 0 && page > 0 ? (unsigned long long)pages * (unsigned long long)page : 0;
}

bool wayland_display_alive() {
  const char *d = getenv("WAYLAND_DISPLAY");
  if (!d || !*d) return true;
  std::string path = d;
  if (path[0] != '/') {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) return true;
    path = std::string(rt) + "/" + path;
  }
  struct sockaddr_un a = {};
  a.sun_family = AF_UNIX;
  if (path.size() >= sizeof a.sun_path) return true;
  memcpy(a.sun_path, path.c_str(), path.size() + 1);
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return true;
  bool ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
  int err = errno;
  close(fd);
  // only a missing socket or one nobody listens on means the session ended
  return ok || (err != ENOENT && err != ECONNREFUSED);
}

} // namespace fld
