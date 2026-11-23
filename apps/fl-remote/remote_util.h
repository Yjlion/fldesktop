// fl-remote: saved connections and the FreeRDP command line (unit tested)
#pragma once
#include <string>
#include <vector>

namespace remote {

struct Connection {
  std::string name, protocol = "vnc"; // vnc or rdp
  std::string host;
  int port = 0;                        // 0: the protocol's default
  std::string user, domain;
  std::string password;                // never saved
};

// remote.ini: [connection:<name>] Protocol= Host= Port= User= Domain=
std::string to_ini(const std::vector<Connection> &conns);
std::vector<Connection> from_ini(const std::string &text);
// "host", "host:5901", "[::1]:5900" -> host, port (0 when none)
void split_host(const std::string &s, std::string &host, int &port);
// vnc://host:port, rdp://user@host, or a bare host (VNC)
bool parse_url(const std::string &url, Connection &c);
// the FreeRDP client's arguments (FreeRDP 2 and 3 take the same /options)
std::vector<std::string> freerdp_args(const std::string &client, const Connection &c);
// clipboard text for VNC (Latin-1); other characters become '?'
std::string utf8_to_latin1(const std::string &s);

} // namespace remote
