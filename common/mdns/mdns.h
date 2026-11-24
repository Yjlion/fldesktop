// mDNS / DNS-SD browsing through the Avahi daemon (avahi-client on the
// fld GLib thread). Services found are reported in the FLTK thread
// (Fl::lock() in main()). When avahi-daemon is not running nothing is
// found; the browser starts working when it comes up.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "mdns_util.h"

namespace mdns {

class Browser {
public:
  // types: {"_smb._tcp", "_sftp-ssh._tcp", ...}. added() gets resolved
  // services (each one once, the first address found), removed() the ones
  // that went away.
  Browser(std::vector<std::string> types, std::function<void(const Service &)> added,
          std::function<void(const Service &)> removed = nullptr);
  ~Browser();
  Browser(const Browser &) = delete;
  Browser &operator=(const Browser &) = delete;
  // the services known now
  std::vector<Service> services() const;

  struct Impl;

private:
  std::shared_ptr<Impl> impl_;
};

// Whether avahi-daemon answers (blocks briefly; call off the UI thread or once)
bool daemon_running();

} // namespace mdns
