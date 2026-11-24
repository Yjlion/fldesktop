// Settings > Sharing: the computer's name and what Avahi (mDNS) announces.
// Plain logic, shared by the page and fl-settings-helper (unit tested).
#pragma once
#include <string>
#include <vector>

namespace sharing {

struct Published {
  const char *id;      // "ssh": /etc/avahi/services/fldesktop-ssh.service
  const char *label;   // "Remote login (SSH)"
  const char *type;    // "_ssh._tcp"
  int port;
  // a program that must be installed for it to make sense (any of them)
  std::vector<const char *> needs;
};
const std::vector<Published> &published();
const Published *find_published(const std::string &id);
// the Avahi service file announcing it under the computer's name
std::string service_xml(const Published &p);
std::string service_path(const std::string &id, const std::string &dir = "/etc/avahi/services");

// RFC 1123 host names: labels of letters, digits and '-' (not at their ends,
// 1-63 each), joined by dots
bool valid_hostname(const std::string &name);
// a host name from a pretty name ("Leo's Laptop" -> "leos-laptop")
std::string hostname_from(const std::string &pretty);
// /etc/hosts with the 127.0.1.1 line naming `name` (Debian's convention)
std::string hosts_with_name(const std::string &hosts, const std::string &old_name, const std::string &name);

} // namespace sharing
