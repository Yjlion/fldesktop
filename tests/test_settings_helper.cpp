// fl-settings-helper: the command allowlist and argument checks (helper_args)
#include <string>
#include <vector>

#include "helper_args.h"
#include "test.h"

using fld::helper::check;

static bool allowed(std::vector<std::string> a) { return check(a).empty(); }

int main() {
  // the commands Settings runs
  CHECK(allowed({"system", "greeter-ini", "/tmp/greeter.ini"}));
  CHECK(allowed({"system", "greeter-ini", "/tmp/greeter.ini", "/home/a/b g.png"}));
  CHECK(allowed({"system", "timezone", "Europe/Berlin"}));
  CHECK(allowed({"system", "timezone", "America/Argentina/Buenos_Aires"}));
  CHECK(allowed({"system", "timezone", "Etc/GMT+5"}));
  CHECK(allowed({"system", "set-time", "2026-11-26 12:30:00"}));
  CHECK(allowed({"system", "set-time", "2026-11-26 12:30"}));
  CHECK(allowed({"system", "ntp", "on"}));
  CHECK(allowed({"system", "avahi", "off"}));
  CHECK(allowed({"system", "hostname", "my-laptop"}));
  CHECK(allowed({"system", "publish", "ssh", "on"}));
  CHECK(allowed({"users", "add", "alice", "Alice Smith", "1"}));
  CHECK(allowed({"users", "delete", "alice", "remove-home"}));
  CHECK(allowed({"users", "modify", "alice", "Alice", "0", "audio,video,lp"}));
  CHECK(allowed({"users", "modify", "alice", "Alice", "0", "-"}));
  CHECK(allowed({"users", "password", "alice"}));
  CHECK(allowed({"users", "picture", "alice", "/home/alice/me.png"}));
  CHECK(allowed({"users", "picture", "alice", "-"}));
  CHECK(allowed({"users", "autologin", "-"}));
  CHECK(allowed({"users", "autologin", "bob"}));

  // not on the list
  CHECK(!allowed({}));
  CHECK(!allowed({"system"}));
  CHECK(!allowed({"greeter-ini", "/tmp/x"}));        // the old form, without a group
  CHECK(!allowed({"users", "timezone", "Europe/Berlin"})); // command of the other group
  CHECK(!allowed({"system", "add", "alice", "A", "1"}));
  CHECK(!allowed({"system", "shell", "/bin/sh"}));
  CHECK(!allowed({"root", "add", "x", "y", "1"}));

  // arity
  CHECK(!allowed({"system", "timezone"}));
  CHECK(!allowed({"system", "timezone", "Europe/Berlin", "extra"}));
  CHECK(!allowed({"system", "greeter-ini", "/a", "/b", "/c"}));
  CHECK(!allowed({"users", "add", "alice", "Alice"}));
  CHECK(!allowed({"users", "modify", "alice", "Alice", "0"}));

  // paths
  CHECK(!allowed({"system", "greeter-ini", "relative.ini"}));
  CHECK(!allowed({"system", "greeter-ini", ""}));
  CHECK(!allowed({"system", "greeter-ini", "/tmp/a\nb"}));
  CHECK(!allowed({"system", "greeter-ini", "/tmp/a", "-rf"}));
  CHECK(!allowed({"users", "picture", "alice", "me.png"}));
  CHECK(!allowed({"system", "greeter-ini", "/" + std::string(5000, 'a')}));

  // time zones and times
  CHECK(!allowed({"system", "timezone", "../../etc/shadow"}));
  CHECK(!allowed({"system", "timezone", "/etc/shadow"}));
  CHECK(!allowed({"system", "timezone", "Europe/../../x"}));
  CHECK(!allowed({"system", "timezone", "Europe/Berlin;id"}));
  CHECK(!allowed({"system", "timezone", ""}));
  CHECK(!allowed({"system", "set-time", "tomorrow"}));
  CHECK(!allowed({"system", "set-time", "2026-13-01 00:00"}));
  CHECK(!allowed({"system", "set-time", "-s 2026-01-01"}));

  // switches and services
  CHECK(!allowed({"system", "ntp", "yes"}));
  CHECK(!allowed({"system", "avahi", "--now"}));
  CHECK(!allowed({"system", "publish", "telnet", "on"}));
  CHECK(!allowed({"system", "publish", "ssh", "1"}));
  CHECK(!allowed({"system", "hostname", "-rf"}));
  CHECK(!allowed({"system", "hostname", "a b"}));
  CHECK(!allowed({"system", "hostname", "x;reboot"}));

  // user names, full names, groups, flags
  CHECK(!allowed({"users", "add", "root", "Root", "1"}));
  CHECK(!allowed({"users", "add", "-oroot", "x", "1"}));
  CHECK(!allowed({"users", "add", "Alice", "Alice", "1"}));
  CHECK(!allowed({"users", "add", "al:ice", "Alice", "1"}));
  CHECK(!allowed({"users", "add", "alice", "Alice:0:0", "1"}));
  CHECK(!allowed({"users", "add", "alice", "Alice\nroot", "1"}));
  CHECK(!allowed({"users", "add", "alice", "Alice", "yes"}));
  CHECK(!allowed({"users", "delete", "alice", "everything"}));
  CHECK(!allowed({"users", "delete", "../etc", "keep"}));
  CHECK(!allowed({"users", "modify", "alice", "Alice", "0", "wheel,-G"}));
  CHECK(!allowed({"users", "modify", "alice", "Alice", "0", "a,,b"}));
  CHECK(!allowed({"users", "modify", "alice", "Alice", "0", "a b"}));
  CHECK(!allowed({"users", "modify", "alice", "Alice", "0", ""}));
  CHECK(!allowed({"users", "autologin", ""}));
  CHECK(!allowed({"users", "password", "nobody"}));

  // the reason is readable
  CHECK(check({"system", "nope"}).find("unknown command") != std::string::npos);
  CHECK(check({"system", "ntp"}).find("wrong number") != std::string::npos);

  return test_result("settings-helper");
}
