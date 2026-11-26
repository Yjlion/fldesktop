// Local crash reports. fld::init_app() installs a handler for fatal signals
// (SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL) that writes the program, its
// version and a backtrace to $XDG_STATE_HOME/fldesktop/crashes/ and then lets
// the signal go on (systemd-coredump still gets the core). fl-logs lists the
// reports and can open a prefilled bug report.
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace fld {

void install_crash_handler(const char *program);
std::string crash_dir();

struct CrashReport {
  std::string path, program, signal, version;
  time_t when = 0;
  std::string text; // the whole report
};
// the reports, newest first
std::vector<CrashReport> crash_reports();
bool parse_crash_report(const std::string &text, CrashReport &out);
// a GitHub "new issue" URL with the report as the body (shortened to fit a URL)
std::string crash_issue_url(const CrashReport &r, const std::string &repo = "Yjlion/fldesktop");

} // namespace fld
