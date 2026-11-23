#include "keyring_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

#include "fld/util.h"

namespace keyring {

bool parse_ssh_fingerprint(const std::string &line, SshKey &k) {
  std::istringstream in(fld::trim(line));
  std::string bits, fp;
  if (!(in >> bits >> fp)) return false;
  if (bits.find_first_not_of("0123456789") != std::string::npos || fp.find(':') == std::string::npos) return false;
  k.bits = atoi(bits.c_str());
  k.fingerprint = fp;
  std::string rest;
  std::getline(in, rest);
  rest = fld::trim(rest);
  // "(TYPE)" at the end, the comment before it
  if (!rest.empty() && rest.back() == ')') {
    size_t open = rest.rfind('(');
    if (open != std::string::npos) {
      k.type = rest.substr(open + 1, rest.size() - open - 2);
      rest = fld::trim(rest.substr(0, open));
    }
  }
  k.comment = rest == "no comment" ? "" : rest;
  return true;
}

std::vector<SshKey> find_ssh_keys(const std::string &dir) {
  std::vector<SshKey> out;
  for (const std::string &n : fld::list_dir(dir)) {
    if (!fld::ends_with(n, ".pub")) continue;
    SshKey k;
    k.pub_path = dir + "/" + n;
    std::string priv = dir + "/" + n.substr(0, n.size() - 4);
    if (fld::file_exists(priv)) k.path = priv;
    // the comment from the .pub (type, key, comment)
    std::string text;
    if (fld::read_file(k.pub_path, text, 64 * 1024)) {
      std::istringstream in(text);
      std::string type, data, comment;
      in >> type >> data;
      std::getline(in, comment);
      k.comment = fld::trim(comment);
      if (fld::starts_with(type, "ssh-")) k.type = fld::lower(type.substr(4));
      else if (fld::starts_with(type, "ecdsa-")) k.type = "ecdsa";
      else k.type = type;
      for (char &c : k.type) c = (char)toupper((unsigned char)c);
    }
    out.push_back(k);
  }
  std::sort(out.begin(), out.end(), [](const SshKey &a, const SshKey &b) { return a.pub_path < b.pub_path; });
  return out;
}

std::string gpg_unescape(const std::string &s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '\\' && i + 3 < s.size() && s[i + 1] == 'x' && isxdigit((unsigned char)s[i + 2]) &&
        isxdigit((unsigned char)s[i + 3])) {
      o += (char)strtol(s.substr(i + 2, 2).c_str(), nullptr, 16);
      i += 3;
    } else {
      o += s[i];
    }
  }
  return o;
}

static std::string algo_name(int a, const std::string &bits, const std::string &curve) {
  std::string n;
  switch (a) {
    case 1: case 2: case 3: n = "RSA"; break;
    case 16: n = "ElGamal"; break;
    case 17: n = "DSA"; break;
    case 18: n = "ECDH"; break;
    case 19: n = "ECDSA"; break;
    case 22: n = "EdDSA"; break;
    default: n = "Algorithm " + std::to_string(a); break;
  }
  if (!curve.empty()) return n + " (" + curve + ")";
  return bits.empty() || bits == "0" ? n : n + " " + bits;
}

std::vector<GpgKey> parse_gpg_colons(const std::string &out) {
  std::vector<GpgKey> keys;
  bool in_primary = false; // fpr lines after pub/sec belong to the primary key
  for (const std::string &line : fld::split(out, '\n')) {
    std::vector<std::string> f = fld::split(line, ':', true);
    if (f.empty()) continue;
    const std::string &t = f[0];
    if (t == "pub" || t == "sec") {
      GpgKey k;
      k.secret = t == "sec";
      std::string validity = f.size() > 1 ? f[1] : "";
      k.revoked = validity == "r";
      k.expired = validity == "e";
      k.key_id = f.size() > 4 ? f[4] : "";
      k.created = f.size() > 5 ? atoll(f[5].c_str()) : 0;
      k.expires = f.size() > 6 ? atoll(f[6].c_str()) : 0;
      k.algorithm = algo_name(f.size() > 3 ? atoi(f[3].c_str()) : 0, f.size() > 2 ? f[2] : "",
                              f.size() > 16 ? f[16] : "");
      keys.push_back(k);
      in_primary = true;
    } else if (t == "fpr" && in_primary && !keys.empty()) {
      if (f.size() > 9 && keys.back().fingerprint.empty()) keys.back().fingerprint = f[9];
      in_primary = false;
    } else if (t == "uid" && !keys.empty()) {
      if (f.size() > 9 && !(f.size() > 1 && f[1] == "r")) keys.back().uids.push_back(gpg_unescape(f[9]));
    } else if (t == "sub" || t == "ssb") {
      in_primary = false;
    }
  }
  return keys;
}

void mark_secret(std::vector<GpgKey> &keys, const std::vector<GpgKey> &secret) {
  for (GpgKey &k : keys)
    for (const GpgKey &s : secret)
      if (!k.fingerprint.empty() && k.fingerprint == s.fingerprint) k.secret = true;
}

bool valid_key_name(const std::string &name) {
  if (name.empty() || name.size() > 64 || name[0] == '.' || name[0] == '-') return false;
  for (char c : name)
    if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
  return !fld::ends_with(name, ".pub");
}

std::string attributes_text(const std::vector<std::pair<std::string, std::string>> &attrs) {
  std::string o;
  for (auto &a : attrs) {
    // the schema name says little
    if (a.first == "xdg:schema") continue;
    o += (o.empty() ? "" : ", ") + a.first + ": " + a.second;
  }
  return o;
}

} // namespace keyring
