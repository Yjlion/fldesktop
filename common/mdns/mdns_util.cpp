#include "mdns_util.h"

#include <cctype>

namespace mdns {

std::map<std::string, std::string> parse_txt(const std::vector<std::string> &records) {
  std::map<std::string, std::string> m;
  for (const std::string &r : records) {
    size_t eq = r.find('=');
    std::string k = r.substr(0, eq), v = eq == std::string::npos ? "" : r.substr(eq + 1);
    for (char &c : k) c = (char)tolower((unsigned char)c);
    if (!k.empty() && !m.count(k)) m[k] = v; // the first one counts
  }
  return m;
}

std::string url_host(const Service &s) {
  if (!s.host.empty()) return s.host;
  if (s.address.find(':') != std::string::npos) {
    // a link-local address needs its zone, percent-encoded in a URL
    std::string a = s.address;
    size_t pct = a.find('%');
    if (pct != std::string::npos) a.replace(pct, 1, "%25");
    return "[" + a + "]";
  }
  return s.address;
}

static std::string path_from_txt(const Service &s, const char *key) {
  auto it = s.txt.find(key);
  std::string p = it == s.txt.end() ? "" : it->second;
  if (!p.empty() && p[0] == '/') p = p.substr(1);
  return p;
}

std::string service_url(const Service &s) {
  std::string h = url_host(s);
  if (h.empty()) return "";
  auto port = [&](int def) { return s.port && s.port != def ? ":" + std::to_string(s.port) : std::string(); };
  const std::string &t = s.type;
  if (t == "_smb._tcp") return "smb://" + h + "/";
  if (t == "_sftp-ssh._tcp" || t == "_ssh._tcp") return "sftp://" + h + port(22) + "/";
  if (t == "_webdav._tcp") return "dav://" + h + port(80) + "/" + path_from_txt(s, "path");
  if (t == "_webdavs._tcp") return "davs://" + h + port(443) + "/" + path_from_txt(s, "path");
  if (t == "_ftp._tcp") return "ftp://" + h + port(21) + "/" + path_from_txt(s, "path");
  if (t == "_nfs._tcp") return "nfs://" + h + "/" + path_from_txt(s, "path");
  if (t == "_ipp._tcp") return "ipp://" + h + port(631) + "/" + path_from_txt(s, "rp");
  if (t == "_ipps._tcp") return "ipps://" + h + port(631) + "/" + path_from_txt(s, "rp");
  if (t == "_rfb._tcp") return "vnc://" + h + port(5900);
  if (t == "_rdp._tcp") return "rdp://" + h + port(3389);
  if (t == "_uscan._tcp") return "http://" + h + port(80) + "/" + path_from_txt(s, "rs");
  if (t == "_uscans._tcp") return "https://" + h + port(443) + "/" + path_from_txt(s, "rs");
  return "";
}

std::string type_label(const std::string &t) {
  static const std::map<std::string, std::string> labels = {
      {"_smb._tcp", "Windows share"}, {"_sftp-ssh._tcp", "SFTP"}, {"_ssh._tcp", "SSH"},
      {"_webdav._tcp", "WebDAV"},     {"_webdavs._tcp", "WebDAV (secure)"}, {"_ftp._tcp", "FTP"},
      {"_nfs._tcp", "NFS share"},     {"_ipp._tcp", "Printer"}, {"_ipps._tcp", "Printer (secure)"},
      {"_rfb._tcp", "Remote desktop (VNC)"}, {"_rdp._tcp", "Remote desktop (RDP)"},
      {"_uscan._tcp", "Scanner"},     {"_uscans._tcp", "Scanner (secure)"}};
  auto it = labels.find(t);
  return it == labels.end() ? t : it->second;
}

std::string printer_model(const Service &s) {
  auto it = s.txt.find("ty");
  if (it != s.txt.end() && !it->second.empty()) return it->second;
  it = s.txt.find("product");
  if (it != s.txt.end()) {
    std::string p = it->second;
    if (p.size() >= 2 && p.front() == '(' && p.back() == ')') p = p.substr(1, p.size() - 2);
    return p;
  }
  return "";
}

} // namespace mdns
