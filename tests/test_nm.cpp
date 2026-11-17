// helpers of the NetworkManager library (no running NetworkManager needed)
#include "nmclient.h"
#include "test.h"

int main() {
  std::string a;
  unsigned p;
  CHECK(nm::parse_address("192.168.1.10/24", a, p, false));
  CHECK_EQ(a, std::string("192.168.1.10"));
  CHECK_EQ(p, 24u);
  CHECK(nm::parse_address("10.0.0.1", a, p, false));
  CHECK_EQ(p, 24u); // default prefix
  CHECK(!nm::parse_address("10.0.0.300/24", a, p, false));
  CHECK(!nm::parse_address("10.0.0.1/33", a, p, false));
  CHECK(!nm::parse_address("10.0.0.1/x", a, p, false));
  CHECK(nm::parse_address("2001:db8::5/64", a, p, true));
  CHECK_EQ(p, 64u);
  CHECK(!nm::parse_address("2001:db8::5/129", a, p, true));
  CHECK(!nm::parse_address("192.168.1.1", a, p, true));
  auto l = nm::split_list(" 1.1.1.1, 8.8.8.8\n9.9.9.9;");
  CHECK_EQ(l.size(), 3u);
  CHECK_EQ(l[2], std::string("9.9.9.9"));
  CHECK_EQ(std::string(nm::signal_icon(90)), std::string("network-wireless-signal-excellent"));
  CHECK_EQ(std::string(nm::signal_icon(30)), std::string("network-wireless-signal-ok"));
  CHECK_EQ(std::string(nm::signal_icon(-1)), std::string("network-wireless-signal-none"));

  // WireGuard (wg-quick) files; keys from RFC 7748
  const std::string priv = "dwdtCnMYpX08FsFyUbJmRd9ML4frwJkqsXf7pR25LCo=";
  const std::string pub = "hSDwCYkwp1R0i33ctD73Wg2/Og0mOBr066SpjqqbTmo=";
  std::string conf = "# a comment\n"
                     "[Interface]\n"
                     "PrivateKey = " + priv + "\n"
                     "Address = 10.8.0.2/32, fd86:ea04:1115::2/128\n"
                     "DNS = 10.8.0.1, fd86:ea04:1115::1, vpn.lan\n"
                     "ListenPort = 51820\n"
                     "PostUp = iptables -A FORWARD  # wg-quick only\n"
                     "\n"
                     "[Peer]\n"
                     "PublicKey = " + pub + "\n"
                     "PresharedKey = " + pub + "\n"
                     "AllowedIPs = 0.0.0.0/0, ::/0\n"
                     "Endpoint = vpn.example.com:51820\n"
                     "PersistentKeepalive = 25\n";
  nm::WgConfig wc;
  std::string err;
  CHECK(nm::parse_wg_quick(conf, wc, err));
  CHECK_EQ(wc.private_key, priv);
  CHECK_EQ(wc.listen_port, 51820);
  CHECK_EQ(wc.addresses.size(), (size_t)2);
  CHECK_EQ(wc.dns.size(), (size_t)2);
  CHECK_EQ(wc.dns_search.size(), (size_t)1);
  CHECK_EQ(wc.peers.size(), (size_t)1);
  if (!wc.peers.empty()) {
    CHECK_EQ(wc.peers[0].public_key, pub);
    CHECK_EQ(wc.peers[0].allowed_ips, std::string("0.0.0.0/0,::/0"));
    CHECK_EQ(wc.peers[0].endpoint, std::string("vpn.example.com:51820"));
    CHECK_EQ(wc.peers[0].keepalive, 25);
  }
  CHECK(!nm::parse_wg_quick("[Interface]\nAddress = 10.0.0.1/24\n", wc, err)); // no key
  CHECK(!nm::parse_wg_quick("[Interface]\nPrivateKey = " + priv + "\n[Peer]\nPublicKey = nope\n", wc, err));
  CHECK(!nm::parse_wg_quick("[Bogus]\n", wc, err));
  CHECK(nm::valid_wg_key(priv));
  CHECK(!nm::valid_wg_key("short="));
  CHECK_EQ(nm::wg_iface_name("/home/u/My VPN.conf"), std::string("My-VPN"));
  CHECK_EQ(nm::wg_iface_name("averyveryverylongname.conf"), std::string("averyveryverylo"));
  CHECK_EQ(nm::wg_iface_name("/x/!!!.conf"), std::string("wg0"));
  if (nm::wg_keys_supported()) {
    CHECK_EQ(nm::wg_public_key(priv), pub);
    std::string k = nm::wg_generate_private_key();
    CHECK(nm::valid_wg_key(k));
    CHECK(nm::valid_wg_key(nm::wg_public_key(k)));
    CHECK(k != nm::wg_generate_private_key());
  }
  // VPN plugin data
  auto kv = nm::parse_kv("remote = vpn.example.com\n# c\n\nport=1194\nbad line\n");
  CHECK_EQ(kv.size(), (size_t)2);
  CHECK_EQ(kv[1].second, std::string("1194"));
  CHECK_EQ(nm::format_kv(kv), std::string("remote = vpn.example.com\nport = 1194\n"));
  return test_result("nm");
}
