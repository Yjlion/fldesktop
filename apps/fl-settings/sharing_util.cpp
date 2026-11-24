#include "sharing_util.h"

#include <cctype>
#include <sstream>

namespace sharing {

const std::vector<Published> &published() {
  static const std::vector<Published> v = {
      {"ssh", "Remote login (SSH)", "_ssh._tcp", 22, {"/usr/sbin/sshd", "/usr/bin/sshd"}},
      {"sftp", "File transfer (SFTP)", "_sftp-ssh._tcp", 22, {"/usr/sbin/sshd", "/usr/bin/sshd"}},
      {"vnc", "Screen sharing (VNC)", "_rfb._tcp", 5900, {"/usr/bin/wayvnc", "/usr/bin/fl-remote-server"}},
      {"smb", "Windows file sharing (Samba)", "_smb._tcp", 445, {"/usr/sbin/smbd", "/usr/bin/smbd"}},
  };
  return v;
}

const Published *find_published(const std::string &id) {
  for (auto &p : published())
    if (id == p.id) return &p;
  return nullptr;
}

std::string service_xml(const Published &p) {
  return std::string("<?xml version=\"1.0\" standalone='no'?>\n"
                     "<!DOCTYPE service-group SYSTEM \"avahi-service.dtd\">\n"
                     "<!-- written by Settings > Sharing (fldesktop) -->\n"
                     "<service-group>\n"
                     "  <name replace-wildcards=\"yes\">%h</name>\n"
                     "  <service>\n"
                     "    <type>") +
         p.type + "</type>\n    <port>" + std::to_string(p.port) +
         "</port>\n  </service>\n</service-group>\n";
}

std::string service_path(const std::string &id, const std::string &dir) {
  return dir + "/fldesktop-" + id + ".service";
}

static bool valid_label(const std::string &n) {
  if (n.empty() || n.size() > 63 || n.front() == '-' || n.back() == '-') return false;
  for (char c : n)
    if (!isalnum((unsigned char)c) && c != '-') return false;
  return true;
}

bool valid_hostname(const std::string &n) {
  if (n.empty() || n.size() > 253) return false;
  size_t a = 0;
  for (;;) { // dot-separated labels (a full name like box.example.org)
    size_t dot = n.find('.', a);
    if (!valid_label(n.substr(a, dot == std::string::npos ? std::string::npos : dot - a))) return false;
    if (dot == std::string::npos) return true;
    a = dot + 1;
  }
}

std::string hostname_from(const std::string &pretty) {
  std::string r;
  for (char c : pretty) {
    if (isalnum((unsigned char)c)) r += (char)tolower((unsigned char)c);
    else if (c == '\'') continue;
    else if (!r.empty() && r.back() != '-') r += '-';
  }
  while (!r.empty() && r.back() == '-') r.pop_back();
  if (r.size() > 63) r.resize(63);
  while (!r.empty() && r.back() == '-') r.pop_back();
  return r;
}

std::string hosts_with_name(const std::string &hosts, const std::string &old_name, const std::string &name) {
  std::istringstream in(hosts);
  std::string line, out;
  bool done = false;
  while (std::getline(in, line)) {
    std::istringstream l(line);
    std::string addr;
    l >> addr;
    if (addr == "127.0.1.1" && !done) {
      out += "127.0.1.1\t" + name + "\n";
      done = true;
      continue;
    }
    // the old name on other lines (127.0.0.1 localhost oldname): replaced
    if (!old_name.empty() && !addr.empty() && addr[0] != '#') {
      std::string word, rebuilt = addr;
      bool changed = false;
      while (l >> word) {
        if (word == old_name) {
          word = name;
          changed = true;
        }
        rebuilt += "\t" + word;
      }
      if (changed) {
        out += rebuilt + "\n";
        continue;
      }
    }
    out += line + "\n";
  }
  if (!done) out += "127.0.1.1\t" + name + "\n";
  return out;
}

} // namespace sharing
