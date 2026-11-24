// The login keyring's key, from the login to the daemon.
//
// At login there is no session bus yet: the PAM module runs
// "fl-secretd --login" as the user with the password on stdin. It unlocks
// (or makes) the login keyring, then waits in the background on a socket in
// $XDG_RUNTIME_DIR/fldesktop (0700) and hands the derived key to the first
// fl-secretd of the same user that asks (SO_PEERCRED), then exits; after
// a few minutes without one it exits anyway, forgetting the key.
#pragma once
#include <string>

#include "crypto.h"

namespace stash {

std::string socket_path(); // "" without XDG_RUNTIME_DIR
// blocks: serves the key once (or until timeout_s); false if it can't listen
bool serve(const crypto::Bytes &key, int timeout_s);
// the key, when a login left one
bool fetch(crypto::Bytes &key);

} // namespace stash
