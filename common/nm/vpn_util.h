// VPN helpers without libnm (unit tested): WireGuard wg-quick files and keys,
// key = value text for VPN plugin data.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace nm {

struct WgPeer {
  std::string public_key, preshared_key, endpoint, allowed_ips; // allowed_ips: comma separated
  int keepalive = 0;                                           // seconds, 0 = off
};

struct WgConfig {
  std::string private_key;
  int listen_port = 0, mtu = 0;
  std::vector<std::string> addresses; // "10.0.0.2/32", "fd00::2/128"
  std::vector<std::string> dns;       // servers
  std::vector<std::string> dns_search; // search domains given in DNS=
  std::vector<WgPeer> peers;
};

// Parses a wg-quick configuration ([Interface] and [Peer] sections).
bool parse_wg_quick(const std::string &text, WgConfig &out, std::string &err);
// A valid interface name from a file name ("/x/My VPN.conf" -> "My-VPN"),
// at most 15 characters; "wg0" when nothing is left.
std::string wg_iface_name(const std::string &path);
// A WireGuard key: base64 of 32 bytes.
bool valid_wg_key(const std::string &key);
// New private key / the public key of a private key ("" when not built with
// libcrypto or the key is invalid).
std::string wg_generate_private_key();
std::string wg_public_key(const std::string &private_key);
bool wg_keys_supported();

// "key = value" lines <-> pairs (blank lines and # comments are skipped)
std::vector<std::pair<std::string, std::string>> parse_kv(const std::string &text);
std::string format_kv(const std::vector<std::pair<std::string, std::string>> &kv);

// An IPv6 address (contains ':')
bool is_ipv6(const std::string &addr);

} // namespace nm
