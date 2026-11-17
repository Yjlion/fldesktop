// firewalld helpers: port parsing, texts
#include "fwclient.h"
#include "test.h"

int main() {
  std::string port, proto;
  CHECK(fw::parse_port("8080/tcp", port, proto));
  CHECK_EQ(port, std::string("8080"));
  CHECK_EQ(proto, std::string("tcp"));
  CHECK(fw::parse_port(" 27000-27100/UDP ", port, proto));
  CHECK_EQ(port, std::string("27000-27100"));
  CHECK_EQ(proto, std::string("udp"));
  CHECK(fw::parse_port("443", port, proto));
  CHECK_EQ(proto, std::string("tcp"));
  CHECK(!fw::parse_port("0/tcp", port, proto));
  CHECK(!fw::parse_port("70000/tcp", port, proto));
  CHECK(!fw::parse_port("100-50/tcp", port, proto));
  CHECK(!fw::parse_port("http/tcp", port, proto));
  CHECK(!fw::parse_port("80/icmp", port, proto));
  fw::ForwardPort f{"8080", "tcp", "80", "192.168.1.5"};
  CHECK_EQ(f.text(), std::string("8080/tcp → 192.168.1.5:80"));
  fw::ForwardPort g{"2222", "tcp", "22", ""};
  CHECK_EQ(g.text(), std::string("2222/tcp → port 22"));
  CHECK_EQ(fw::error_text("INVALID_PORT: 99999"), std::string("The port is not valid."));
  CHECK_EQ(fw::error_text("org.freedesktop.DBus.Error.ServiceUnknown"),
           std::string("The firewall service (firewalld) is not running."));
  CHECK_EQ(fw::target_text("ACCEPT"), std::string("Accept all incoming connections"));
  fw::State st;
  fw::Zone z;
  z.name = "home";
  st.zones.push_back(z);
  CHECK(st.zone("home") != nullptr);
  CHECK(st.zone("work") == nullptr);
  return test_result("firewall");
}
