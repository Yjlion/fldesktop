// fl-logs: journal JSON, syslog lines, priorities and filters
#include <ctime>

#include "fld/util.h"
#include "logs_util.h"
#include "test.h"

using namespace logs;

int main() {
  Entry e;
  CHECK(parse_journal_json(
      "{\"__REALTIME_TIMESTAMP\":\"1700000000123456\",\"PRIORITY\":\"3\",\"SYSLOG_IDENTIFIER\":\"sshd\","
      "\"_SYSTEMD_UNIT\":\"sshd.service\",\"_PID\":\"812\",\"_HOSTNAME\":\"box\",\"MESSAGE\":\"Failed password\"}",
      e));
  CHECK(e.time > 1700000000.12 && e.time < 1700000000.13);
  CHECK_EQ(e.priority, ERR);
  CHECK_EQ(e.source, std::string("sshd"));
  CHECK_EQ(e.unit, std::string("sshd.service"));
  CHECK_EQ(e.pid, 812);
  CHECK_EQ(e.message, std::string("Failed password"));
  CHECK_EQ(e.fields["_HOSTNAME"], std::string("box"));
  // a binary message is an array of bytes; no identifier: the command
  Entry b;
  CHECK(parse_journal_json("{\"MESSAGE\":[104,105],\"_COMM\":\"app\"}", b));
  CHECK_EQ(b.message, std::string("hi"));
  CHECK_EQ(b.source, std::string("app"));
  CHECK_EQ(b.priority, INFO);
  Entry k;
  CHECK(parse_journal_json("{\"MESSAGE\":\"usb 1-1: new device\",\"_TRANSPORT\":\"kernel\",\"PRIORITY\":\"6\"}", k));
  CHECK_EQ(k.source, std::string("kernel"));
  CHECK(!parse_journal_json("not json", e));

  Entry s = parse_syslog_line("Nov  2 10:01:02 box dhcpcd[441]: eth0: leased 10.0.0.5", 2026);
  CHECK_EQ(s.source, std::string("dhcpcd"));
  CHECK_EQ(s.pid, 441);
  CHECK_EQ(s.host, std::string("box"));
  CHECK_EQ(s.message, std::string("eth0: leased 10.0.0.5"));
  time_t t = (time_t)s.time;
  struct tm tm;
  localtime_r(&t, &tm);
  CHECK_EQ(tm.tm_mon, 10);
  CHECK_EQ(tm.tm_mday, 2);
  CHECK_EQ(tm.tm_hour, 10);
  CHECK_EQ(tm.tm_year, 126);
  Entry r = parse_syslog_line("2026-11-22T09:30:00.250+01:00 box kernel: error: disk sda failed", 2026);
  CHECK_EQ(r.source, std::string("kernel"));
  CHECK_EQ(r.priority, ERR);
  CHECK_EQ(r.message, std::string("error: disk sda failed"));
  Entry o = parse_syslog_line(" * Starting sshd ... [ ok ]", 2026); // OpenRC rc.log
  CHECK_EQ(o.time, 0.0);
  CHECK_EQ(o.message, std::string(" * Starting sshd ... [ ok ]"));

  CHECK_EQ(guess_priority("Warning: low memory"), WARNING);
  CHECK_EQ(guess_priority("CRITICAL temperature"), CRIT);
  CHECK_EQ(guess_priority("service failed to start"), ERR);
  CHECK_EQ(guess_priority("all fine"), INFO);
  CHECK_EQ(guess_priority("terrorism"), INFO); // whole words only
  CHECK_EQ(std::string(priority_name(WARNING)), std::string("Warning"));

  Filter f;
  CHECK(matches(e, f));
  f.max_priority = WARNING;
  CHECK(matches(e, f));
  f.max_priority = CRIT;
  CHECK(!matches(e, f));
  f = Filter();
  f.text = "PASSWORD";
  CHECK(matches(e, f));
  f.text = "nothing";
  CHECK(!matches(e, f));
  f = Filter();
  f.source = "sshd.service";
  CHECK(matches(e, f));
  f.source = "cron";
  CHECK(!matches(e, f));

  time_t now = time(nullptr);
  CHECK_EQ(format_time(0, now), std::string(""));
  CHECK_EQ(format_time((double)now, now).size(), (size_t)8); // HH:MM:SS today

  char tmpl[] = "/tmp/fld-logs-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  for (const char *n : {"syslog", "syslog.1", "messages.2.gz", "Xorg.0.log", "wtmp", "kern.log", "README"})
    fld::write_file(dir + "/" + n, "x");
  auto files = log_files(dir);
  CHECK_EQ(files.size(), (size_t)3);
  CHECK_EQ(fld::basename(files[0]), std::string("Xorg.0.log"));
  CHECK_EQ(fld::basename(files[2]), std::string("syslog"));
  std::string out;
  fld::run_capture({"rm", "-r", dir}, out);
  return test_result("logs");
}
