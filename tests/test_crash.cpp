// libfld crash reports: the handler writes a report, parse and URL
#include <csignal>
#include <cstdlib>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fld/crash.h"
#include "fld/util.h"
#include "test.h"

int main() {
  char tmpl[] = "/tmp/fld-crash-XXXXXX";
  std::string t = mkdtemp(tmpl);
  setenv("XDG_STATE_HOME", t.c_str(), 1);
  unsetenv("FLD_NO_CRASH_HANDLER");
  CHECK_EQ(fld::crash_dir(), t + "/fldesktop/crashes");
  pid_t pid = fork();
  if (pid == 0) {
    // no core file from this test
    struct rlimit rl = {0, 0};
    setrlimit(RLIMIT_CORE, &rl);
    fld::install_crash_handler("/usr/bin/fl-test-crash");
    raise(SIGSEGV);
    _exit(0);
  }
  int st = 0;
  waitpid(pid, &st, 0);
  CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV); // the signal goes on
  auto reps = fld::crash_reports();
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define NO_HANDLER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define NO_HANDLER 1
#endif
#ifndef NO_HANDLER
  CHECK_EQ(reps.size(), (size_t)1);
  if (!reps.empty()) {
    CHECK_EQ(reps[0].program, std::string("fl-test-crash"));
    CHECK_EQ(reps[0].signal, std::string("SIGSEGV"));
    CHECK(reps[0].text.find("Backtrace:") != std::string::npos);
    std::string url = fld::crash_issue_url(reps[0]);
    CHECK(url.find("https://github.com/Yjlion/fldesktop/issues/new?labels=bug,crash&title=fl-test-crash") == 0);
    CHECK(url.find("SIGSEGV") != std::string::npos);
    CHECK(url.size() < 20000);
  }
#endif
  fld::CrashReport r;
  CHECK(!fld::parse_crash_report("nothing", r));
  CHECK(fld::parse_crash_report("Program: x\nVersion: 1\nSignal: SIGABRT\n", r));
  CHECK_EQ(r.version, std::string("1"));
  std::string out;
  fld::run_capture({"rm", "-rf", t}, out);
  return test_result("crash");
}
