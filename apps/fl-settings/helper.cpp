// fl-settings-helper: the parts of Settings that need root, run with pkexec.
//
//   fl-settings-helper greeter-ini <new.ini> [<background image>]
//
// Writes /etc/fldesktop/greeter.ini with the validated keys of <new.ini>.
// A background image is copied to /etc/fldesktop/greeter-background.<ext>
// (the greeter cannot read the user's home). Both files must belong to the
// user who ran pkexec.
//
// Date & Time on systems without systemd-timedated (Alpine, Devuan):
//   fl-settings-helper timezone <Region/City>   /etc/localtime (+ /etc/timezone)
//   fl-settings-helper set-time "YYYY-MM-DD HH:MM:SS"   date -s, hwclock -w
//   fl-settings-helper ntp on|off   starts and enables (stops and disables)
//                                   chronyd, ntpd or openntpd
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "fld/ini.h"
#include "fld/util.h"
#include "datetime_sys.h"
#include "greeter_config.h"
#include "helper_util.h"

using fld::helper_fail;
static int fail(const std::string &m) { return helper_fail(m); }

static int set_timezone(const std::string &zone) {
  if (!dtsys::valid_zone(zone)) return fail("unknown time zone: " + zone);
  std::string target = "/usr/share/zoneinfo/" + zone;
  unlink("/etc/localtime.new");
  if (symlink(target.c_str(), "/etc/localtime.new") != 0 || rename("/etc/localtime.new", "/etc/localtime") != 0)
    return fail(std::string("cannot link /etc/localtime: ") + strerror(errno));
  // Debian and Devuan also keep the name in /etc/timezone
  if (fld::file_exists("/etc/timezone") || fld::file_exists("/etc/debian_version"))
    if (!fld::write_atomic("/etc/timezone", zone + "\n")) return fail("cannot write /etc/timezone");
  return 0;
}

static int set_time(const std::string &t) {
  if (!dtsys::valid_time(t)) return fail("not a date and time: " + t);
  std::string out;
  if (fld::run_capture({"date", "-s", t}, out) != 0) return fail("date -s failed: " + out);
  if (!fld::which("hwclock").empty()) fld::run_capture({"hwclock", "-w"}, out); // best effort
  return 0;
}

static int set_ntp(bool on) {
  std::string svc = dtsys::installed_ntp_service();
  if (svc.empty()) return fail("No network time service is installed (chrony, ntpd or openntpd).");
  for (auto &cmd : dtsys::service_commands(dtsys::detect_init(), svc, on)) {
    std::string out;
    if (fld::run_capture(cmd, out) != 0 && cmd[0] != "update-rc.d") return fail(cmd[0] + " failed: " + out);
  }
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 3 && geteuid() != 0) return fail("must run as root (through pkexec)");
  if (argc == 3) {
    if (!strcmp(argv[1], "timezone")) return set_timezone(argv[2]);
    if (!strcmp(argv[1], "set-time")) return set_time(argv[2]);
    if (!strcmp(argv[1], "ntp") && (!strcmp(argv[2], "on") || !strcmp(argv[2], "off")))
      return set_ntp(!strcmp(argv[2], "on"));
  }
  if (argc < 3 || strcmp(argv[1], "greeter-ini") != 0)
    return fail("usage: fl-settings-helper greeter-ini <new.ini> [<background image>] | timezone <zone> | "
                "set-time <time> | ntp on|off");
  uid_t uid = fld::helper_caller_uid();

  std::string text, err;
  if (!fld::read_user_file(argv[2], uid, 64 * 1024, text, err)) return fail(err);
  fld::Ini in;
  if (!in.parse(text)) return fail("cannot read the new settings");

  mkdir("/etc/fldesktop", 0755);
  fld::Ini out;
  out.load(fld::greeter_ini_path());  // keeps other keys and comments
  if (!fld::validate_greeter_config(in, out, err)) return fail(err);

  if (argc > 3) {
    std::string img;
    if (!fld::read_user_file(argv[3], uid, 32u << 20, img, err)) return fail(err);
    std::string ext = fld::image_type(img);
    if (ext.empty()) return fail("the background is not a PNG, JPEG or SVG image");
    for (const char *e : {"png", "jpg", "svg"})
      if (ext != e) unlink((std::string("/etc/fldesktop/greeter-background.") + e).c_str());
    std::string dest = "/etc/fldesktop/greeter-background." + ext;
    if (!fld::write_atomic(dest, img)) return fail("cannot write " + dest);
    out.set("Greeter", "Background", dest);
  }
  std::string ini_path = fld::greeter_ini_path();
  if (!fld::write_atomic(ini_path, out.to_string())) return fail("cannot write " + ini_path);
  return 0;
}
