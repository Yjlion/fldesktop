// fl-remote: DES and VNC authentication, the RFB handshake and updates,
// connections and FreeRDP arguments
#include <cstring>

#include "remote_util.h"
#include "rfb.h"
#include "test.h"

static std::string hex(const std::string &s) {
  std::string o;
  char b[4];
  for (unsigned char c : s) {
    snprintf(b, sizeof b, "%02x", c);
    o += b;
  }
  return o;
}

static std::string be16(int v) { return std::string{(char)(v >> 8), (char)(v & 255)}; }
static std::string be32(uint32_t v) { return be16((int)(v >> 16)) + be16((int)(v & 0xffff)); }

int main() {
  uint8_t key[8] = {0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1};
  uint8_t in[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF}, out[8];
  rfb::des_encrypt(key, in, out);
  CHECK_EQ(hex(std::string((char *)out, 8)), std::string("85e813540f0ab405"));
  // the response is two DES blocks with the bit-reversed password as key
  std::string ch(16, '\0');
  std::string r = rfb::vnc_auth_response(ch, "");
  CHECK_EQ(hex(r), std::string("8ca64de9c1b123a78ca64de9c1b123a7"));
  CHECK_EQ(rfb::vnc_auth_response(ch, "password").size(), (size_t)16);
  CHECK(rfb::vnc_auth_response(ch, "password") == rfb::vnc_auth_response(ch, "password+more")); // 8 chars count

  // a 3.8 server with a VNC password
  {
    rfb::Client c("secret");
    std::string init = "RFB 003.008\n";
    CHECK(c.feed(init.data(), init.size()));
    CHECK_EQ(c.take_output(), std::string("RFB 003.008\n"));
    std::string types = std::string("\x01\x02", 2);
    c.feed(types.data(), types.size());
    CHECK_EQ(c.take_output(), std::string("\x02", 1));
    std::string challenge = "0123456789abcdef";
    c.feed(challenge.data(), 16);
    CHECK_EQ(c.take_output(), rfb::vnc_auth_response(challenge, "secret"));
    std::string ok = be32(0);
    c.feed(ok.data(), 4);
    CHECK_EQ(c.take_output(), std::string("\x01", 1)); // shared
    bool connected = false;
    c.on_connected = [&] { connected = true; };
    // ServerInit: 4x2, a pixel format, the name "test"
    std::string si = be16(4) + be16(2) + std::string(16, '\0') + be32(4) + "test";
    c.feed(si.data(), si.size());
    CHECK(connected);
    CHECK_EQ(c.state(), rfb::Client::NORMAL);
    CHECK_EQ(c.width(), 4);
    CHECK_EQ(c.name(), std::string("test"));
    std::string msgs = c.take_output();
    CHECK_EQ((int)msgs[0], 0);                        // SetPixelFormat
    CHECK_EQ((int)(unsigned char)msgs[20], 2);       // SetEncodings
    CHECK_EQ((int)(unsigned char)msgs[msgs.size() - 10], 3); // FramebufferUpdateRequest
    // an update: a raw 2x1 rect at (1,1), then a CopyRect of it to (0,0)
    int updates = 0;
    c.on_update = [&](int, int, int, int) { updates++; };
    std::string up = std::string("\x00\x00", 2) + be16(2) + be16(1) + be16(1) + be16(2) + be16(1) + be32(0) +
                     std::string("\x30\x20\x10\x00\x03\x02\x01\x00", 8) + be16(0) + be16(0) + be16(2) + be16(1) +
                     be32(1) + be16(1) + be16(1);
    // in two pieces: the parser waits for the rest
    c.feed(up.data(), 10);
    c.feed(up.data() + 10, up.size() - 10);
    CHECK_EQ(updates, 2);
    CHECK_EQ(c.pixels()[1 * 4 + 1], 0x102030u);
    CHECK_EQ(c.pixels()[1 * 4 + 2], 0x010203u);
    CHECK_EQ(c.pixels()[0], 0x102030u);
    CHECK_EQ(c.pixels()[1], 0x010203u);
    std::string req = c.take_output();
    CHECK_EQ((int)req[0], 3);
    CHECK_EQ((int)req[1], 1); // incremental
    // input
    c.pointer(3, 1, 1);
    CHECK_EQ(c.take_output(), std::string("\x05\x01", 2) + be16(3) + be16(1));
    c.key(0xff0d, true);
    CHECK_EQ(c.take_output(), std::string("\x04\x01\x00\x00", 4) + be32(0xff0d));
    // the server's clipboard, a bell, a resize
    std::string got;
    c.on_cut_text = [&](const std::string &t) { got = t; };
    std::string cut = std::string("\x03\x00\x00\x00", 4) + be32(5) + "hello" + std::string("\x02", 1);
    bool bell = false;
    c.on_bell = [&] { bell = true; };
    c.feed(cut.data(), cut.size());
    CHECK_EQ(got, std::string("hello"));
    CHECK(bell);
    std::string rs = std::string("\x00\x00", 2) + be16(1) + be16(0) + be16(0) + be16(8) + be16(6) + be32((uint32_t)-223);
    c.feed(rs.data(), rs.size());
    CHECK_EQ(c.width(), 8);
    CHECK_EQ(c.height(), 6);
    CHECK_EQ(c.pixels().size(), (size_t)48);
  }
  // a wrong password (3.8 says why)
  {
    rfb::Client c("x");
    std::string s = std::string("RFB 003.008\n") + std::string("\x01\x02", 2) + std::string(16, 'c') + be32(1) +
                    be32(12) + "auth failed!";
    CHECK(!c.feed(s.data(), s.size()));
    CHECK_EQ(c.error(), std::string("auth failed!"));
  }
  // 3.3 without a password; asking for one when needed
  {
    rfb::Client c;
    std::string s = std::string("RFB 003.003\n") + be32(1);
    c.feed(s.data(), s.size());
    CHECK_EQ(c.take_output(), std::string("RFB 003.003\n\x01", 13));
    CHECK_EQ(c.state(), rfb::Client::SERVER_INIT);
    rfb::Client p;
    bool asked = false;
    p.on_need_password = [&] { asked = true; };
    std::string t = std::string("RFB 003.007\n") + std::string("\x01\x02", 2) + std::string(16, 'z');
    p.feed(t.data(), t.size());
    CHECK(asked);
    CHECK(p.waiting_for_password());
    p.take_output();
    p.set_password("pw");
    p.password_given();
    CHECK_EQ(p.take_output(), rfb::vnc_auth_response(std::string(16, 'z'), "pw"));
  }
  {
    rfb::Client c;
    std::string s = "SSH-2.0-OpenSSH_9.6\r\n";
    CHECK(!c.feed(s.data(), s.size()));
  }
  CHECK_EQ(rfb::keysym_for(0xff0d, ""), 0xff0du);
  CHECK_EQ(rfb::keysym_for('a', "a"), (uint32_t)'a');
  CHECK_EQ(rfb::keysym_for('a', "A"), (uint32_t)'A');
  CHECK_EQ(rfb::keysym_for(0, "\xc3\xa9"), 0xe9u);        // é is Latin-1
  CHECK_EQ(rfb::keysym_for(0, "\xe2\x82\xac"), 0x10020acu); // €

  using namespace remote;
  std::string h;
  int p;
  split_host("example.org:5901", h, p);
  CHECK(h == "example.org" && p == 5901);
  split_host("[fe80::1]:5900", h, p);
  CHECK(h == "fe80::1" && p == 5900);
  split_host("fe80::1", h, p);
  CHECK(h == "fe80::1" && p == 0);
  Connection c;
  CHECK(parse_url("rdp://alice@win.example.org:3390/", c));
  CHECK(c.protocol == "rdp" && c.user == "alice" && c.host == "win.example.org" && c.port == 3390);
  CHECK(parse_url("vnc://pi.local", c) && c.protocol == "vnc" && c.host == "pi.local" && c.port == 0);
  CHECK(parse_url("10.0.0.2:1", c) && c.protocol == "vnc" && c.port == 1);
  CHECK(!parse_url("http://x", c));
  std::vector<Connection> list(2);
  list[0].name = "Pi";
  list[0].host = "pi.local";
  list[1].name = "Office";
  list[1].protocol = "rdp";
  list[1].host = "pc";
  list[1].user = "bob";
  list[1].domain = "CORP";
  list[1].password = "never-saved";
  std::string ini = to_ini(list);
  CHECK(ini.find("never-saved") == std::string::npos);
  auto back = from_ini(ini);
  CHECK_EQ(back.size(), (size_t)2);
  CHECK_EQ(back[0].name, std::string("Pi"));
  CHECK_EQ(back[1].domain, std::string("CORP"));
  auto args = freerdp_args("wlfreerdp3", list[1]);
  CHECK_EQ(args[0], std::string("wlfreerdp3"));
  CHECK_EQ(args[1], std::string("/v:pc"));
  CHECK_EQ(args[2], std::string("/u:bob"));
  CHECK_EQ(args[3], std::string("/d:CORP"));
  CHECK_EQ(args[4], std::string("/p:never-saved"));
  CHECK_EQ(utf8_to_latin1("caf\xc3\xa9 \xe2\x82\xac"), std::string("caf\xe9 ?"));
  return test_result("remote");
}
