#include "fld/crash.h"

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <execinfo.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "fld/util.h"

#ifndef FLD_VERSION
#define FLD_VERSION "unknown"
#endif

namespace fld {

namespace {
// filled in before a crash: the handler may only use async-signal-safe calls
char g_dir[1024];
char g_header[1024];
char g_program[64];
char g_altstack[64 * 1024];

void put(int fd, const char *s) {
  if (s) {
    ssize_t r = write(fd, s, strlen(s));
    (void)r;
  }
}

void put_num(int fd, unsigned long v) {
  char buf[24];
  int i = sizeof buf;
  buf[--i] = 0;
  do buf[--i] = (char)('0' + v % 10); while ((v /= 10) && i > 0);
  put(fd, buf + i);
}

const char *sig_name(int sig) {
  switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
  }
  return "signal";
}

void on_crash(int sig) {
  // the file: <dir>/<program>-<time>-<pid>.txt
  char path[1200];
  size_t n = 0;
  auto app = [&](const char *s) {
    for (; *s && n + 1 < sizeof path; s++) path[n++] = *s;
  };
  app(g_dir);
  app("/");
  app(g_program);
  app("-");
  char num[24];
  int i = sizeof num;
  num[--i] = 0;
  unsigned long t = (unsigned long)time(nullptr);
  do num[--i] = (char)('0' + t % 10); while ((t /= 10) && i > 0);
  app(num + i);
  app(".txt");
  path[n] = 0;
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd >= 0) {
    put(fd, g_header);
    put(fd, "Signal: ");
    put(fd, sig_name(sig));
    put(fd, "\nPID: ");
    put_num(fd, (unsigned long)getpid());
    put(fd, "\n\nBacktrace:\n");
    void *frames[64];
    int nf = backtrace(frames, 64);
    backtrace_symbols_fd(frames, nf, fd);
    close(fd);
  }
  // on to the default action (core dump)
  signal(sig, SIG_DFL);
  raise(sig);
}
} // namespace

std::string crash_dir() {
  std::string base = getenv_or("XDG_STATE_HOME", "");
  if (base.empty()) base = home_dir() + "/.local/state";
  return base + "/fldesktop/crashes";
}

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define FLD_ASAN 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define FLD_ASAN 1
#endif

void install_crash_handler(const char *program) {
#ifdef FLD_ASAN
  (void)program;
  return; // AddressSanitizer reports crashes itself
#endif
  if (getenv("FLD_NO_CRASH_HANDLER")) return;
  std::string dir = crash_dir();
  mkdir_p(dir, 0700);
  snprintf(g_dir, sizeof g_dir, "%s", dir.c_str());
  snprintf(g_program, sizeof g_program, "%s", basename(program ? program : "program").c_str());
  struct utsname u;
  std::string sys = uname(&u) == 0 ? std::string(u.sysname) + " " + u.release + " " + u.machine : "";
  std::string os;
  std::string rel;
  if (read_file("/etc/os-release", rel))
    for (auto &l : split(rel, '\n'))
      if (starts_with(l, "PRETTY_NAME=")) os = trim(l.substr(12));
  if (os.size() >= 2 && os.front() == '"') os = os.substr(1, os.size() - 2);
  snprintf(g_header, sizeof g_header, "Program: %s\nVersion: %s\nSystem: %s\nDistribution: %s\n", g_program,
           FLD_VERSION, sys.c_str(), os.c_str());
  // load libgcc's unwinder now: backtrace() may allocate the first time
  void *warm[2];
  backtrace(warm, 2);
  stack_t ss;
  ss.ss_sp = g_altstack;
  ss.ss_size = sizeof g_altstack;
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = on_crash;
  sa.sa_flags = SA_ONSTACK | SA_RESETHAND;
  sigemptyset(&sa.sa_mask);
  for (int s : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) sigaction(s, &sa, nullptr);
}

bool parse_crash_report(const std::string &text, CrashReport &out) {
  out.text = text;
  for (auto &l : split(text, '\n')) {
    if (starts_with(l, "Program: ")) out.program = l.substr(9);
    else if (starts_with(l, "Version: ")) out.version = l.substr(9);
    else if (starts_with(l, "Signal: ")) out.signal = l.substr(8);
  }
  return !out.program.empty() && !out.signal.empty();
}

std::vector<CrashReport> crash_reports() {
  std::vector<CrashReport> out;
  std::string dir = crash_dir();
  for (auto &n : list_dir(dir)) {
    if (!ends_with(n, ".txt")) continue;
    std::string p = dir + "/" + n, text;
    if (!read_file(p, text, 256 * 1024)) continue;
    CrashReport r;
    if (!parse_crash_report(text, r)) continue;
    r.path = p;
    struct stat st;
    r.when = stat(p.c_str(), &st) == 0 ? st.st_mtime : 0;
    out.push_back(r);
  }
  std::sort(out.begin(), out.end(), [](const CrashReport &a, const CrashReport &b) { return a.when > b.when; });
  return out;
}

static std::string url_encode(const std::string &s) {
  static const char *hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

std::string crash_issue_url(const CrashReport &r, const std::string &repo) {
  std::string title = r.program + " crashed (" + r.signal + ")";
  std::string body = "**What were you doing when it crashed?**\n\n\n**Crash report**\n\n```\n";
  std::string rep = r.text;
  // a URL has to stay below about 8 KB
  if (rep.size() > 5000) rep = rep.substr(0, 5000) + "\n[...]";
  body += rep + "\n```\n";
  return "https://github.com/" + repo + "/issues/new?labels=bug,crash&title=" + url_encode(title) +
         "&body=" + url_encode(body);
}

} // namespace fld
