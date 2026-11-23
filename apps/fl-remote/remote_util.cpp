#include "remote_util.h"

#include <cstdlib>

#include "fld/ini.h"
#include "fld/util.h"

namespace remote {

std::string to_ini(const std::vector<Connection> &conns) {
  fld::Ini ini;
  for (const Connection &c : conns) {
    std::string g = "connection:" + c.name;
    ini.set(g, "Protocol", c.protocol);
    ini.set(g, "Host", c.host);
    if (c.port) ini.set_int(g, "Port", c.port);
    if (!c.user.empty()) ini.set(g, "User", c.user);
    if (!c.domain.empty()) ini.set(g, "Domain", c.domain);
  }
  return ini.to_string();
}

std::vector<Connection> from_ini(const std::string &text) {
  fld::Ini ini;
  ini.parse(text);
  std::vector<Connection> out;
  for (const std::string &g : ini.groups()) {
    if (!fld::starts_with(g, "connection:")) continue;
    Connection c;
    c.name = g.substr(11);
    c.protocol = fld::lower(ini.get(g, "Protocol", "vnc")) == "rdp" ? "rdp" : "vnc";
    c.host = ini.get(g, "Host", "");
    c.port = ini.get_int(g, "Port", 0);
    c.user = ini.get(g, "User", "");
    c.domain = ini.get(g, "Domain", "");
    if (!c.host.empty()) out.push_back(c);
  }
  return out;
}

void split_host(const std::string &in, std::string &host, int &port) {
  std::string s = fld::trim(in);
  port = 0;
  if (!s.empty() && s[0] == '[') {
    size_t e = s.find(']');
    host = s.substr(1, e == std::string::npos ? std::string::npos : e - 1);
    if (e != std::string::npos && e + 1 < s.size() && s[e + 1] == ':') port = atoi(s.c_str() + e + 2);
    return;
  }
  size_t colon = s.rfind(':');
  // one colon: host:port; more: a bare IPv6 address
  if (colon != std::string::npos && s.find(':') == colon) {
    host = s.substr(0, colon);
    port = atoi(s.c_str() + colon + 1);
  } else {
    host = s;
  }
}

bool parse_url(const std::string &url, Connection &c) {
  std::string u = fld::trim(url);
  c = Connection();
  if (fld::starts_with(u, "vnc://")) {
    u = u.substr(6);
  } else if (fld::starts_with(u, "rdp://")) {
    c.protocol = "rdp";
    u = u.substr(6);
  } else if (u.find("://") != std::string::npos) {
    return false;
  }
  if (!u.empty() && u.back() == '/') u.pop_back();
  size_t at = u.rfind('@');
  if (at != std::string::npos) {
    c.user = fld::uri_decode(u.substr(0, at));
    u = u.substr(at + 1);
  }
  split_host(u, c.host, c.port);
  c.name = c.host;
  return !c.host.empty() && c.host.find_first_of(" /") == std::string::npos;
}

std::vector<std::string> freerdp_args(const std::string &client, const Connection &c) {
  std::vector<std::string> a = {client, "/v:" + c.host + (c.port ? ":" + std::to_string(c.port) : "")};
  if (!c.user.empty()) a.push_back("/u:" + c.user);
  if (!c.domain.empty()) a.push_back("/d:" + c.domain);
  if (!c.password.empty()) a.push_back("/p:" + c.password);
  a.push_back("/dynamic-resolution");
  a.push_back("+clipboard");
  // accept a new server's certificate once, then hold it to it (trust on first use)
  a.push_back("/cert:tofu");
  a.push_back("/title:" + c.name);
  return a;
}

std::string utf8_to_latin1(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
      o += (char)c;
    } else if ((c & 0xe0) == 0xc0 && i + 1 < s.size()) {
      unsigned cp = ((c & 0x1f) << 6) | ((unsigned char)s[i + 1] & 0x3f);
      o += cp <= 0xff ? (char)cp : '?';
      i++;
    } else {
      o += '?';
      while (i + 1 < s.size() && ((unsigned char)s[i + 1] & 0xc0) == 0x80) i++;
    }
  }
  return o;
}

} // namespace remote
