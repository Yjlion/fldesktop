#include "proxy_env.h"

#include <cctype>

#include "fld/ini.h"
#include "fld/util.h"

namespace fld {

static const char *kLower[] = {"http_proxy", "https_proxy", "ftp_proxy", "all_proxy", "no_proxy"};

const std::vector<std::string> &proxy_variables() {
  static std::vector<std::string> v;
  if (v.empty()) {
    for (auto *k : kLower) v.push_back(k);
    for (auto *k : kLower) {
      std::string u = k;
      for (auto &c : u) c = (char)toupper((unsigned char)c);
      v.push_back(u);
    }
  }
  return v;
}

ProxySettings load_proxy(const Ini &ini) {
  ProxySettings p;
  std::string m = lower(ini.get("Proxy", "Mode", "inherit"));
  p.mode = m == "none" ? ProxySettings::None : m == "manual" ? ProxySettings::Manual : ProxySettings::Inherit;
  p.http = ini.get("Proxy", "Http", "");
  p.https = ini.get("Proxy", "Https", "");
  p.ftp = ini.get("Proxy", "Ftp", "");
  p.socks = ini.get("Proxy", "Socks", "");
  p.use_http_for_all = ini.get_bool("Proxy", "UseHttpForAll", false);
  p.no_proxy = ini.get("Proxy", "NoProxy", p.no_proxy);
  return p;
}

void save_proxy(Ini &ini, const ProxySettings &p) {
  ini.set("Proxy", "Mode", p.mode == ProxySettings::None ? "none" : p.mode == ProxySettings::Manual ? "manual" : "inherit");
  ini.set("Proxy", "Http", p.http);
  ini.set("Proxy", "Https", p.https);
  ini.set("Proxy", "Ftp", p.ftp);
  ini.set("Proxy", "Socks", p.socks);
  ini.set_bool("Proxy", "UseHttpForAll", p.use_http_for_all);
  ini.set("Proxy", "NoProxy", p.no_proxy);
}

std::map<std::string, std::string> inherited_proxy(const std::map<std::string, std::string> &env) {
  std::map<std::string, std::string> out;
  for (auto &k : proxy_variables()) {
    auto it = env.find(k);
    if (it != env.end() && !it->second.empty()) out[k] = it->second;
  }
  return out;
}

ProxySettings::Mode default_mode(const std::map<std::string, std::string> &inherited) {
  for (auto &kv : inherited)
    if (lower(kv.first) != "no_proxy") return ProxySettings::Inherit;
  return ProxySettings::None;
}

std::string proxy_url(const std::string &value, const std::string &scheme) {
  std::string v = trim(value);
  if (v.empty()) return "";
  if (v.find("://") != std::string::npos) return v;
  return scheme + "://" + v;
}

bool valid_proxy(const std::string &value) {
  std::string v = trim(value);
  if (v.empty()) return true;
  for (char c : v)
    if (isspace((unsigned char)c) || c == '"' || c == '\'' || c == '\\' || (unsigned char)c < 32) return false;
  size_t s = v.find("://");
  if (s != std::string::npos) {
    std::string scheme = lower(v.substr(0, s));
    if (scheme != "http" && scheme != "https" && scheme != "socks" && scheme != "socks4" && scheme != "socks4a" &&
        scheme != "socks5" && scheme != "socks5h")
      return false;
    v = v.substr(s + 3);
  }
  if (!v.empty() && v.back() == '/') v.pop_back();
  size_t at = v.rfind('@');
  if (at != std::string::npos) v = v.substr(at + 1);
  if (v.empty() || v.find('/') != std::string::npos) return false;
  std::string host = v, port;
  if (v[0] == '[') {  // [ipv6]:port
    size_t e = v.find(']');
    if (e == std::string::npos) return false;
    host = v.substr(1, e - 1);
    if (e + 1 < v.size()) {
      if (v[e + 1] != ':') return false;
      port = v.substr(e + 2);
      if (port.empty()) return false;
    }
  } else {
    size_t c = v.rfind(':');
    if (c != std::string::npos) {
      host = v.substr(0, c);
      port = v.substr(c + 1);
      if (port.empty()) return false;
    }
  }
  if (host.empty()) return false;
  for (char ch : port)
    if (!isdigit((unsigned char)ch)) return false;
  if (!port.empty() && (port.size() > 5 || std::stoi(port) == 0 || std::stoi(port) > 65535)) return false;
  return true;
}

std::vector<std::pair<std::string, std::string>> proxy_environment(const ProxySettings &p) {
  std::map<std::string, std::string> low;
  if (p.mode == ProxySettings::Manual) {
    std::string http = proxy_url(p.http, "http");
    low["http_proxy"] = http;
    low["https_proxy"] = p.use_http_for_all ? http : proxy_url(p.https, "http");
    low["ftp_proxy"] = p.use_http_for_all ? http : proxy_url(p.ftp, "http");
    low["all_proxy"] = p.use_http_for_all ? "" : proxy_url(p.socks, "socks5");
    bool any = !http.empty() || !low["https_proxy"].empty() || !low["ftp_proxy"].empty() || !low["all_proxy"].empty();
    // no_proxy without a proxy would be noise
    std::string np;
    for (auto &h : split(p.no_proxy, ',')) {
      std::string t = trim(h);
      if (!t.empty()) np += (np.empty() ? "" : ",") + t;
    }
    low["no_proxy"] = any ? np : "";
  }
  std::vector<std::pair<std::string, std::string>> out;
  for (auto &k : proxy_variables()) {
    std::string lk = lower(k);
    if (p.mode == ProxySettings::Inherit) out.push_back({k, "@inherit"});
    else if (p.mode == ProxySettings::None) out.push_back({k, ""});
    else out.push_back({k, low[lk]});
  }
  return out;
}

}  // namespace fld
