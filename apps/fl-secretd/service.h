// fl-secretd: the Secret Service (org.freedesktop.secrets) over D-Bus.
#pragma once
#include <functional>
#include <string>

#include "crypto.h"

namespace secretd {

struct Options {
  std::string dir;   // the keyrings: ~/.local/share/fldesktop/keyrings
  std::string self;  // this program (run with --ask for the password dialogs)
};

// Serves until another program takes the name or the bus goes; on_ready
// once the name is ours.
int run(const Options &opt, std::function<void()> on_ready);
// while running: unlock a collection with its key (from the login)
bool unlock_with_key(const std::string &collection, const crypto::Bytes &key);
// a keyring file made by someone else meanwhile (the login's): load it
void reload_collection(const std::string &name);

} // namespace secretd
