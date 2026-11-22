// GVfs helpers that need no GIO (and are unit tested)
#include <cctype>
#include <cstdlib>
#include <unistd.h>

#include "fld/util.h"
#include "gvclient.h"

namespace gv {

const char *const kCancelled = "cancelled";

std::string fuse_root() {
  const char *rt = getenv("XDG_RUNTIME_DIR");
  std::string r = rt && *rt ? rt : "/run/user/" + std::to_string(getuid());
  return r + "/gvfs";
}

bool is_fuse_path(const std::string &path, const std::string &root) {
  return path.size() > root.size() + 1 && path.compare(0, root.size() + 1, root + "/") == 0;
}

std::string uri_scheme(const std::string &uri) {
  size_t c = uri.find(':');
  if (c == std::string::npos || c == 0) return "";
  for (size_t i = 0; i < c; i++) {
    char ch = uri[i];
    if (!(isalnum((unsigned char)ch) || ch == '+' || ch == '-' || ch == '.') || (i == 0 && !isalpha((unsigned char)ch)))
      return "";
  }
  return fld::lower(uri.substr(0, c));
}

bool looks_like_uri(const std::string &s) {
  std::string sc = uri_scheme(s);
  if (sc.empty() || sc == "file" || sc == "trash") return false;
  // "smb://...", or "network:" style locations without a host
  return s.compare(sc.size(), 3, "://") == 0 || sc == "network";
}

const Mount *State::mount_for_path(const std::string &path) const {
  const Mount *best = nullptr;
  for (auto &m : mounts) {
    if (m.path.empty()) continue;
    if ((path == m.path || fld::starts_with(path, m.path + "/")) && (!best || m.path.size() > best->path.size()))
      best = &m;
  }
  return best;
}

std::string display_name_for_uri(const std::string &uri) {
  std::string sc = uri_scheme(uri);
  if (sc.empty()) return uri;
  std::string rest = uri.substr(sc.size() + 1);
  if (fld::starts_with(rest, "//")) rest = rest.substr(2);
  size_t slash = rest.find('/');
  std::string auth = rest.substr(0, slash), path = slash == std::string::npos ? "" : rest.substr(slash);
  std::string host = auth, user;
  size_t at = auth.rfind('@');
  if (at != std::string::npos) {
    user = fld::uri_decode(auth.substr(0, at));
    size_t semi = user.find(';'); // smb: domain;user
    if (semi != std::string::npos) user = user.substr(semi + 1);
    host = auth.substr(at + 1);
  }
  // the port is not part of the name (keep IPv6 brackets)
  size_t colon = host.rfind(':');
  if (colon != std::string::npos && host.find(']', colon) == std::string::npos) host = host.substr(0, colon);
  host = fld::uri_decode(host);
  std::vector<std::string> comps;
  for (auto &c : fld::split(path, '/'))
    if (!c.empty()) comps.push_back(fld::uri_decode(c));
  if (host.empty()) return comps.empty() ? sc : comps.back();
  if (comps.empty()) return user.empty() ? host : user + "@" + host;
  return comps.back() + " on " + host;
}

std::string uri_for_path(const State &s, const std::string &path) {
  const Mount *m = s.mount_for_path(path);
  if (!m || m->uri.empty()) return "";
  std::string uri = m->uri;
  std::string rest = path.substr(m->path.size());
  if (rest.empty()) return uri;
  if (!uri.empty() && uri.back() == '/') uri.pop_back();
  return uri + fld::uri_encode_path(rest);
}

} // namespace gv
