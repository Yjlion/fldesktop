// Proxy settings: desktop.ini [Proxy] <-> the session's proxy variables
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace fld {
class Ini;

struct ProxySettings {
  enum Mode { Inherit, None, Manual } mode = Inherit;
  std::string http, https, ftp, socks;  // host:port or a URL
  bool use_http_for_all = false;        // https and ftp use the HTTP proxy
  std::string no_proxy = "localhost,127.0.0.1,::1";
};

// The variables managed here, lower case first: http_proxy, https_proxy,
// ftp_proxy, all_proxy, no_proxy, then the upper case ones.
const std::vector<std::string> &proxy_variables();

ProxySettings load_proxy(const Ini &ini);
void save_proxy(Ini &ini, const ProxySettings &p);

// The proxy variables set in `env` (any case), e.g. from the session snapshot.
std::map<std::string, std::string> inherited_proxy(const std::map<std::string, std::string> &env);
// No [Proxy] Mode saved yet: Inherit when the session came with proxy
// variables, else None.
ProxySettings::Mode default_mode(const std::map<std::string, std::string> &inherited);

// "proxy:3128" -> "http://proxy:3128"; URLs are kept; "" stays "".
std::string proxy_url(const std::string &value, const std::string &scheme = "http");
// host[:port] or scheme://[user[:pass]@]host[:port][/] ; "" is valid (= not set)
bool valid_proxy(const std::string &value);

// wm.ini [environment] entries for these settings: every variable of
// proxy_variables() with a value, "" (unset) or "@inherit".
std::vector<std::pair<std::string, std::string>> proxy_environment(const ProxySettings &p);

}  // namespace fld
