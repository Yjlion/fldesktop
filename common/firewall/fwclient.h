// firewalld access for fldesktop (GDBus on the fld-glib thread).
//
// Changes go to the running firewall and, unless `permanent` is false, to
// the permanent configuration as well. Callbacks run in the FLTK thread
// (call Fl::lock() in main()).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace fw {

struct ForwardPort {
  std::string port, protocol, to_port, to_addr;
  std::string text() const; // "8080/tcp → 192.168.1.5:80"
};

struct Zone {
  std::string name, short_name, description, target;
  std::vector<std::string> services, sources, interfaces;
  std::vector<std::pair<std::string, std::string>> ports; // port (or range), protocol
  std::vector<ForwardPort> forward_ports;
  bool masquerade = false;
  bool active = false;     // has interfaces or sources
};

struct Service {
  std::string name, short_name, description;
  std::vector<std::pair<std::string, std::string>> ports;
};

struct State {
  bool running = false;    // firewalld on the bus and running
  std::string version, default_zone, state;
  bool panic = false;
  std::vector<Zone> zones;
  std::vector<Service> services;
  std::vector<std::string> interfaces; // network interfaces of the system
  const Zone *zone(const std::string &name) const;
  const Service *service(const std::string &name) const;
};

using Done = std::function<void(const std::string &error)>; // "" = success

class Client {
public:
  static Client &get();
  void start();
  const State &state() const { return state_; }
  std::function<void()> on_changed; // FLTK thread

  void set_default_zone(const std::string &zone, Done done = nullptr);
  void set_panic(bool on, Done done = nullptr);
  void reload(Done done = nullptr);
  void runtime_to_permanent(Done done = nullptr);
  void service(const std::string &zone, const std::string &svc, bool on, bool permanent, Done done = nullptr);
  void port(const std::string &zone, const std::string &port, const std::string &proto, bool on, bool permanent,
            Done done = nullptr);
  void masquerade(const std::string &zone, bool on, bool permanent, Done done = nullptr);
  void source(const std::string &zone, const std::string &src, bool on, bool permanent, Done done = nullptr);
  // moves an interface to `zone` ("" = no zone)
  void interface_zone(const std::string &iface, const std::string &zone, bool permanent, Done done = nullptr);
  void forward_port(const std::string &zone, const ForwardPort &fp, bool on, bool permanent, Done done = nullptr);

  State state_; // internal: written in the FLTK thread only
private:
  Client() = default;
};

// helpers (unit tested)
bool parse_port(const std::string &text, std::string &port, std::string &proto); // "8080/tcp", "60000-61000/udp"
std::string error_text(const std::string &message);                              // firewalld's codes in words
std::string target_text(const std::string &target);

} // namespace fw
