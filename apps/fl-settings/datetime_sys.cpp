#include "datetime_sys.h"

#include <cctype>
#include <climits>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include "fld/util.h"

namespace dtsys {

bool valid_zone(const std::string &zone, const std::string &dir) {
  if (zone.empty() || zone.size() > 128 || zone[0] == '/' || zone[0] == '.') return false;
  for (char c : zone)
    if (!(isalnum((unsigned char)c) || c == '/' || c == '_' || c == '-' || c == '+')) return false;
  if (zone.find("..") != std::string::npos || zone.find("//") != std::string::npos) return false;
  struct stat st;
  return stat((dir + "/" + zone).c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool valid_time(const std::string &t) {
  int y, mo, d, h, mi, s = 0;
  char tail;
  int n = sscanf(t.c_str(), "%4d-%2d-%2d %2d:%2d:%2d%c", &y, &mo, &d, &h, &mi, &s, &tail);
  if (n != 5 && n != 6) return false;
  return y >= 1970 && y <= 2200 && mo >= 1 && mo <= 12 && d >= 1 && d <= 31 && h >= 0 && h <= 23 &&
         mi >= 0 && mi <= 59 && s >= 0 && s <= 60;
}

std::string zone_from_link(const std::string &target) {
  size_t p = target.find("zoneinfo/");
  if (p == std::string::npos) return "";
  std::string z = target.substr(p + 9);
  // posix/ and right/ are variants of the same zones
  for (const char *pre : {"posix/", "right/"})
    if (fld::starts_with(z, pre)) z = z.substr(std::string(pre).size());
  return z;
}

std::string current_zone(const std::string &localtime, const std::string &timezone_file) {
  char buf[PATH_MAX];
  ssize_t n = readlink(localtime.c_str(), buf, sizeof buf - 1);
  if (n > 0) {
    buf[n] = 0;
    std::string z = zone_from_link(buf);
    if (!z.empty()) return z;
  }
  std::string t;
  if (fld::read_file(timezone_file, t, 4096)) {
    std::string z = fld::trim(t.substr(0, t.find('\n')));
    if (!z.empty()) return z;
  }
  return "";
}

const std::vector<NtpService> &ntp_services() {
  static const std::vector<NtpService> v = {
      {"chronyd", "chronyd"}, {"chrony", "chronyd"}, {"ntpd", "ntpd"},
      {"openntpd", "ntpd"},   {"ntpsec", "ntpd"},    {"ntp", "ntpd"},
  };
  return v;
}

std::string installed_ntp_service(const std::string &init_dir) {
  for (auto &s : ntp_services())
    if (fld::file_exists(init_dir + "/" + s.service)) return s.service;
  return "";
}

bool ntp_running(const std::string &proc) {
  for (auto &pid : fld::list_dir(proc)) {
    if (pid.empty() || !isdigit((unsigned char)pid[0])) continue;
    std::string comm;
    if (!fld::read_file(proc + "/" + pid + "/comm", comm, 64)) continue;
    comm = fld::trim(comm);
    for (auto &s : ntp_services())
      if (comm == s.process) return true;
  }
  return false;
}

Init detect_init() {
  return !fld::which("rc-service").empty() || fld::file_exists("/run/openrc") ? Init::OpenRC : Init::SysV;
}

std::vector<std::vector<std::string>> service_commands(Init init, const std::string &svc, bool on) {
  if (init == Init::OpenRC) {
    if (on) return {{"rc-update", "add", svc, "default"}, {"rc-service", svc, "start"}};
    return {{"rc-service", svc, "stop"}, {"rc-update", "del", svc, "default"}};
  }
  if (on) return {{"update-rc.d", svc, "defaults"}, {"update-rc.d", svc, "enable"}, {"service", svc, "start"}};
  return {{"service", svc, "stop"}, {"update-rc.d", svc, "disable"}};
}

} // namespace dtsys
