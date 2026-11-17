// /etc/fldesktop/greeter.ini: the settings of fl-greet, the login screen.
// Settings writes it through fl-settings-helper (as root), which accepts only
// the keys and values validate_greeter_config() lets through.
#pragma once
#include <string>
#include <vector>

namespace fld {
class Ini;

const char *greeter_ini_path();  // /etc/fldesktop/greeter.ini

struct GreeterKey {
  const char *group, *key;
  enum Type { Bool, Int, Choice, Color, Name, Names, Path } type;
  int min, max;              // Int
  const char *choices;       // Choice: "a|b|c"
};
const std::vector<GreeterKey> &greeter_keys();

// Copies the known keys of `in` with valid values into `out` (keys that are
// missing in `in` are left alone in `out`). Returns false and sets `err` on
// the first invalid value.
bool validate_greeter_config(const Ini &in, Ini &out, std::string &err);

// Image types a login screen background may have, by file content.
// Returns the extension ("png", "jpg", "svg") or "".
std::string image_type(const std::string &head);

}  // namespace fld
