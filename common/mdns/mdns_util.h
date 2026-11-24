// mDNS / DNS-SD (Avahi): the plain parts, unit tested without Avahi.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace mdns {

struct Service {
  std::string name;      // "Living Room Printer"
  std::string type;      // "_ipp._tcp"
  std::string domain;    // "local"
  std::string host;      // "printer.local"
  std::string address;   // "192.168.1.20" or "fe80::1%eth0"
  int port = 0;
  std::map<std::string, std::string> txt;
  int interface = -1;
  bool ipv6 = false;
  // same service (several interfaces and protocols report it again)
  std::string key() const { return type + "\x1f" + name + "\x1f" + domain; }
};

// "key=value" TXT strings -> map (keys lower-cased, as DNS-SD says)
std::map<std::string, std::string> parse_txt(const std::vector<std::string> &records);
// A URL to open the service with, by its type ("" for types we don't open):
//  _smb._tcp -> smb://host/        _sftp-ssh._tcp -> sftp://host:port/
//  _webdav._tcp -> dav://host:port/path   _webdavs._tcp -> davs://...
//  _ipp._tcp -> ipp://host:port/rp        _ipps._tcp -> ipps://...
//  _rfb._tcp -> vnc://host:port    _uscan._tcp -> http://host:port/rs
std::string service_url(const Service &s);
// host for URLs: "printer.local", or an address ([fe80::1] for IPv6)
std::string url_host(const Service &s);
// what a person reads for a service type ("Windows share", "Printer", ...)
std::string type_label(const std::string &type);
// the printer's make and model from its TXT (ty, else product), "" if none
std::string printer_model(const Service &s);

} // namespace mdns
