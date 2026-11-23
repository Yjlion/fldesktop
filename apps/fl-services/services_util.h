// fl-services: system services from systemd or OpenRC (unit tested parsing)
#pragma once
#include <string>
#include <vector>

namespace svc {

enum class Init { Systemd, OpenRC, Unknown };
Init detect_init(); // /run/systemd/system, then /run/openrc or rc-service

struct Service {
  std::string name;        // sshd (without .service)
  std::string description;
  std::string active;      // active, inactive, failed, activating / started, stopped, crashed
  std::string sub;         // running, exited, dead (systemd)
  std::string enabled;     // enabled, disabled, static, masked / the runlevels (OpenRC)
  bool running() const { return sub == "running" || active == "started" || (active == "active" && sub.empty()); }
  bool failed() const { return active == "failed" || active == "crashed"; }
};

// systemctl list-units --type=service --all --plain --no-legend:
//   "sshd.service loaded active running OpenSSH Daemon"
std::vector<Service> parse_systemctl_units(const std::string &out);
// systemctl list-unit-files --type=service --plain --no-legend:
//   "sshd.service enabled disabled"; fills in `enabled` and adds the ones
//   that are not loaded (inactive)
void merge_unit_files(std::vector<Service> &list, const std::string &out);

// rc-status --all --nocolor:
//   "Runlevel: default\n sshd    [  started  ]\n"
std::vector<Service> parse_rc_status(const std::string &out);
// rc-update show -v: " sshd | boot default" (empty: in no runlevel)
void merge_rc_update(std::vector<Service> &list, const std::string &out);

// A service name a command may take: letters, digits and @ . _ - : \ only.
bool valid_name(const std::string &name);
// "Running", "Stopped", "Failed", "Starting", ...
std::string state_label(const Service &s);

} // namespace svc
