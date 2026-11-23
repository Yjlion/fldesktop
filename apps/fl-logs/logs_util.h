// fl-logs: log entries from the systemd journal (journalctl -o json) and
// from syslog / OpenRC log files, and how they are filtered (unit tested).
#pragma once
#include <ctime>
#include <map>
#include <string>
#include <vector>

namespace logs {

// syslog priorities: 0 emerg .. 7 debug
enum { EMERG, ALERT, CRIT, ERR, WARNING, NOTICE, INFO, DEBUG };
const char *priority_name(int p); // "Error", "Warning", ...

struct Entry {
  double time = 0;        // epoch seconds (with fractions)
  int priority = INFO;
  std::string source;     // the program (SYSLOG_IDENTIFIER, _COMM) or the file
  std::string unit;       // the systemd unit, "" otherwise
  int pid = 0;
  std::string host;
  std::string message;
  std::map<std::string, std::string> fields; // all journal fields (details)
};

// One line of journalctl -o json; false for anything else.
bool parse_journal_json(const std::string &line, Entry &e);
// "Nov 22 10:01:02 host prog[123]: text" (RFC 3164) or an RFC 3339 time
// ("2026-11-22T10:01:02.123+01:00 host prog: text"); `year` fills in the
// missing year of the short form. Lines in no known form keep their text
// with the time 0. The priority is guessed from words like "error".
Entry parse_syslog_line(const std::string &line, int year);
// a guess from the text: "error", "failed", "warning", "critical", ...
int guess_priority(const std::string &text, int def = INFO);

// What to show.
struct Filter {
  int max_priority = DEBUG; // show entries with priority <= this
  std::string text;         // in the message or the source (any case)
  std::string source;       // exactly this source or unit ("" any)
};
bool matches(const Entry &e, const Filter &f);

// "10:01:02" today, "Nov 21 10:01:02" this year, else with the year
std::string format_time(double t, time_t now);

// The log files worth showing in a folder (/var/log): syslog, messages, rc.log,
// *.log, ... without rotated (.1, .gz) copies.
std::vector<std::string> log_files(const std::string &dir);

} // namespace logs
