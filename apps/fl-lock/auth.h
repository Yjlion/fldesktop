// PAM authentication for fl-lock
#pragma once
#include <string>

struct AuthResult {
  bool ok = false;
  std::string message;  // PAM's error / info text for the user ("" = none)
};

// Checks `password` for `user` with the PAM service "fl-lock" (blocking).
AuthResult authenticate(const std::string &user, const std::string &password);
