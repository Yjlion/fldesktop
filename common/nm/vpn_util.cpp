#include "vpn_util.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#ifdef HAVE_LIBCRYPTO
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace nm {

static std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

static std::vector<std::string> split_commas(const std::string &s) {
  std::vector<std::string> v;
  size_t start = 0;
  while (start <= s.size()) {
    size_t e = s.find(',', start);
    std::string p = trim(s.substr(start, e == std::string::npos ? std::string::npos : e - start));
    if (!p.empty()) v.push_back(p);
    if (e == std::string::npos) break;
    start = e + 1;
  }
  return v;
}

static std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

bool is_ipv6(const std::string &a) { return a.find(':') != std::string::npos; }

static bool looks_like_ip(const std::string &s) {
  if (s.empty()) return false;
  if (is_ipv6(s)) return true;
  for (char c : s)
    if (!isdigit((unsigned char)c) && c != '.') return false;
  return std::count(s.begin(), s.end(), '.') == 3;
}

bool parse_wg_quick(const std::string &text, WgConfig &out, std::string &err) {
  out = WgConfig();
  std::string section;
  WgPeer *peer = nullptr;
  int lineno = 0;
  size_t start = 0;
  while (start <= text.size()) {
    size_t e = text.find('\n', start);
    std::string line = text.substr(start, e == std::string::npos ? std::string::npos : e - start);
    start = e == std::string::npos ? text.size() + 1 : e + 1;
    lineno++;
    size_t hash = line.find('#');
    if (hash != std::string::npos) line = line.substr(0, hash);
    line = trim(line);
    if (line.empty()) continue;
    if (line[0] == '[') {
      section = lower(trim(line.substr(1, line.find(']') - 1)));
      if (section == "peer") {
        out.peers.push_back(WgPeer());
        peer = &out.peers.back();
      } else if (section != "interface") {
        err = "line " + std::to_string(lineno) + ": unknown section [" + section + "]";
        return false;
      }
      continue;
    }
    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      err = "line " + std::to_string(lineno) + ": expected key = value";
      return false;
    }
    std::string k = lower(trim(line.substr(0, eq))), v = trim(line.substr(eq + 1));
    if (section == "interface") {
      if (k == "privatekey") out.private_key = v;
      else if (k == "listenport") out.listen_port = atoi(v.c_str());
      else if (k == "mtu") out.mtu = atoi(v.c_str());
      else if (k == "address") for (auto &a : split_commas(v)) out.addresses.push_back(a);
      else if (k == "dns")
        for (auto &d : split_commas(v)) (looks_like_ip(d) ? out.dns : out.dns_search).push_back(d);
      // Table, PreUp, PostUp, SaveConfig, FwMark ...: wg-quick only, ignored
    } else if (section == "peer" && peer) {
      if (k == "publickey") peer->public_key = v;
      else if (k == "presharedkey") peer->preshared_key = v;
      else if (k == "endpoint") peer->endpoint = v;
      else if (k == "allowedips") {
        std::string joined;
        for (auto &a : split_commas(v)) joined += (joined.empty() ? "" : ",") + a;
        peer->allowed_ips += (peer->allowed_ips.empty() || joined.empty() ? "" : ",") + joined;
      } else if (k == "persistentkeepalive") peer->keepalive = v == "off" ? 0 : atoi(v.c_str());
    } else {
      err = "line " + std::to_string(lineno) + ": setting outside of a section";
      return false;
    }
  }
  if (out.private_key.empty()) {
    err = "the [Interface] section has no PrivateKey";
    return false;
  }
  if (!valid_wg_key(out.private_key)) {
    err = "the PrivateKey is not a WireGuard key";
    return false;
  }
  for (auto &p : out.peers)
    if (!valid_wg_key(p.public_key)) {
      err = "a [Peer] has no valid PublicKey";
      return false;
    }
  return true;
}

std::string wg_iface_name(const std::string &path) {
  std::string base = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
  if (base.size() > 5 && lower(base.substr(base.size() - 5)) == ".conf") base.resize(base.size() - 5);
  std::string out;
  for (char c : base) {
    if (isalnum((unsigned char)c) || c == '_' || c == '=' || c == '+' || c == '.' || c == '-') out += c;
    else if (c == ' ' && !out.empty() && out.back() != '-') out += '-';
  }
  while (!out.empty() && out.back() == '-') out.pop_back();
  if (out.size() > 15) out.resize(15);
  return out.empty() ? "wg0" : out;
}

bool valid_wg_key(const std::string &key) {
  if (key.size() != 44 || key[43] != '=') return false;
  for (size_t i = 0; i < 43; i++) {
    char c = key[i];
    if (!isalnum((unsigned char)c) && c != '+' && c != '/') return false;
  }
  return true;
}

std::vector<std::pair<std::string, std::string>> parse_kv(const std::string &text) {
  std::vector<std::pair<std::string, std::string>> v;
  size_t start = 0;
  while (start <= text.size()) {
    size_t e = text.find('\n', start);
    std::string line = trim(text.substr(start, e == std::string::npos ? std::string::npos : e - start));
    start = e == std::string::npos ? text.size() + 1 : e + 1;
    if (line.empty() || line[0] == '#') continue;
    size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    std::string k = trim(line.substr(0, eq));
    if (!k.empty()) v.push_back({k, trim(line.substr(eq + 1))});
  }
  return v;
}

std::string format_kv(const std::vector<std::pair<std::string, std::string>> &kv) {
  std::string s;
  for (auto &p : kv) s += p.first + " = " + p.second + "\n";
  return s;
}

#ifdef HAVE_LIBCRYPTO
bool wg_keys_supported() { return true; }

static std::string b64(const unsigned char *d, size_t n) {
  unsigned char out[64];
  int len = EVP_EncodeBlock(out, d, (int)n);
  return std::string((const char *)out, (size_t)len);
}

std::string wg_generate_private_key() {
  unsigned char k[32];
  if (RAND_bytes(k, sizeof k) != 1) return "";
  // clamping as Curve25519 private keys are
  k[0] &= 248;
  k[31] &= 127;
  k[31] |= 64;
  return b64(k, sizeof k);
}

std::string wg_public_key(const std::string &priv) {
  if (!valid_wg_key(priv)) return "";
  unsigned char raw[48];
  int n = EVP_DecodeBlock(raw, (const unsigned char *)priv.data(), (int)priv.size());
  if (n < 32) return "";
  EVP_PKEY *pk = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, raw, 32);
  if (!pk) return "";
  unsigned char pub[32];
  size_t len = sizeof pub;
  std::string out;
  if (EVP_PKEY_get_raw_public_key(pk, pub, &len) == 1 && len == 32) out = b64(pub, 32);
  EVP_PKEY_free(pk);
  return out;
}
#else
bool wg_keys_supported() { return false; }
std::string wg_generate_private_key() { return ""; }
std::string wg_public_key(const std::string &) { return ""; }
#endif

} // namespace nm
