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
  return test_result("nm");
}
