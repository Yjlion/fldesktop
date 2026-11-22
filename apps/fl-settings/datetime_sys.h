// Date & Time without systemd-timedated (Alpine, Devuan): reading the zone
// and network time state, and what fl-settings-helper runs as root for them.
#pragma once
#include <string>
#include <vector>

namespace dtsys {

// "Europe/Berlin" is a zone under zoneinfo_dir (no "..", no absolute path,
// a regular file or a symlink to one)
bool valid_zone(const std::string &zone, const std::string &zoneinfo_dir = "/usr/share/zoneinfo");
// "YYYY-MM-DD HH:MM[:SS]" with plausible fields
bool valid_time(const std::string &t);

// the zone /etc/localtime points to (".../zoneinfo/Europe/Berlin"), else the
// first line of /etc/timezone, else ""
std::string zone_from_link(const std::string &link_target);
std::string current_zone(const std::string &localtime = "/etc/localtime",
                         const std::string &timezone_file = "/etc/timezone");

// the NTP daemons we know, as their service and process names
struct NtpService { std::string service, process; };
const std::vector<NtpService> &ntp_services();
// the first known NTP service with an init script in init_dir, "" without one
std::string installed_ntp_service(const std::string &init_dir = "/etc/init.d");
// one of the NTP daemons runs (looked up in /proc)
bool ntp_running(const std::string &proc = "/proc");

enum class Init { OpenRC, SysV };
Init detect_init();
// the commands that start and enable (on) or stop and disable (off) a service
std::vector<std::vector<std::string>> service_commands(Init init, const std::string &service, bool on);

} // namespace dtsys
