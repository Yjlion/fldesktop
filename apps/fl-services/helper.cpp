// fl-services-helper: OpenRC service actions as root, run with pkexec.
//   fl-services-helper start|stop|restart <service>
//   fl-services-helper enable|disable <service> [runlevel]   (default: default)
// (systemd services need no helper: systemctl asks polkit itself.)
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "fld/util.h"
#include "services_util.h"

static int fail(const std::string &m) {
  fprintf(stderr, "fl-services-helper: %s\n", m.c_str());
  return 1;
}

int main(int argc, char **argv) {
  if (argc < 3) return fail("usage: fl-services-helper <action> <service> [runlevel]");
  std::string action = argv[1], name = argv[2], level = argc > 3 ? argv[3] : "default";
  if (!svc::valid_name(name)) return fail("not a service name: " + name);
  if (!svc::valid_name(level)) return fail("not a runlevel: " + level);
  // only services that exist
  if (!fld::file_exists("/etc/init.d/" + name)) return fail("no such service: " + name);
  std::vector<std::string> cmd;
  if (action == "start" || action == "stop" || action == "restart") cmd = {"rc-service", name, action};
  else if (action == "enable") cmd = {"rc-update", "add", name, level};
  else if (action == "disable") cmd = {"rc-update", "del", name, level};
  else return fail("unknown action: " + action);
  std::string out;
  int rc = fld::run_capture(cmd, out);
  if (!out.empty()) fprintf(rc ? stderr : stdout, "%s", out.c_str());
  return rc == 0 ? 0 : 1;
}
