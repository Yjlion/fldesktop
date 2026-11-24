// mDNS / DNS-SD helpers: TXT records and the URLs services open with
#include "mdns_util.h"
#include "test.h"

int main() {
  auto txt = mdns::parse_txt({"rp=ipp/print", "TY=HP LaserJet", "ty=second", "flag", "path=/share"});
  CHECK_EQ(txt["rp"], std::string("ipp/print"));
  CHECK_EQ(txt["ty"], std::string("HP LaserJet")); // keys are case-insensitive; the first counts
  CHECK(txt.count("flag") == 1);
  mdns::Service s;
  s.name = "Office";
  s.host = "office.local";
  s.port = 631;
  s.type = "_ipp._tcp";
  s.txt = txt;
  CHECK_EQ(mdns::service_url(s), std::string("ipp://office.local/ipp/print"));
  CHECK_EQ(mdns::printer_model(s), std::string("HP LaserJet"));
  s.type = "_ipps._tcp";
  s.port = 8443;
  CHECK_EQ(mdns::service_url(s), std::string("ipps://office.local:8443/ipp/print"));
  s.type = "_smb._tcp";
  CHECK_EQ(mdns::service_url(s), std::string("smb://office.local/"));
  s.type = "_sftp-ssh._tcp";
  s.port = 22;
  CHECK_EQ(mdns::service_url(s), std::string("sftp://office.local/"));
  s.port = 2222;
  CHECK_EQ(mdns::service_url(s), std::string("sftp://office.local:2222/"));
  s.type = "_webdav._tcp";
  s.port = 80;
  CHECK_EQ(mdns::service_url(s), std::string("dav://office.local/share"));
  s.type = "_rfb._tcp";
  s.port = 5901;
  CHECK_EQ(mdns::service_url(s), std::string("vnc://office.local:5901"));
  s.type = "_unknown._tcp";
  CHECK_EQ(mdns::service_url(s), std::string(""));
  // no host name: the address, IPv6 in brackets with its zone encoded
  mdns::Service v;
  v.type = "_smb._tcp";
  v.address = "fe80::1%eth0";
  CHECK_EQ(mdns::service_url(v), std::string("smb://[fe80::1%25eth0]/"));
  v.address = "192.168.1.5";
  CHECK_EQ(mdns::url_host(v), std::string("192.168.1.5"));
  CHECK_EQ(mdns::type_label("_smb._tcp"), std::string("Windows share"));
  mdns::Service p;
  p.txt = mdns::parse_txt({"product=(Brother HL-L2350DW)"});
  CHECK_EQ(mdns::printer_model(p), std::string("Brother HL-L2350DW"));
  return test_result("mdns");
}
