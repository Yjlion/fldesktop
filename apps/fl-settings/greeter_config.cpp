#include "greeter_config.h"

#include <cctype>
#include <cstdlib>

#include "fld/ini.h"
#include "fld/util.h"

namespace fld {

const char *greeter_ini_path() { return "/etc/fldesktop/greeter.ini"; }

const std::vector<GreeterKey> &greeter_keys() {
  static const std::vector<GreeterKey> k = {
      {"Greeter", "Background", GreeterKey::Path, 0, 0, nullptr},
      {"Greeter", "HideUsers", GreeterKey::Names, 0, 0, nullptr},
      {"Greeter", "MinUid", GreeterKey::Int, 0, 65535, nullptr},
      {"Greeter", "Keyboard", GreeterKey::Choice, 0, 0, "button|always|never"},
      {"Greeter", "KeyboardLayout", GreeterKey::Name, 0, 0, nullptr},
      {"Greeter", "RememberUser", GreeterKey::Bool, 0, 0, nullptr},
      {"Greeter", "ShowUserPictures", GreeterKey::Bool, 0, 0, nullptr},
      {"Greeter", "Clock24", GreeterKey::Bool, 0, 0, nullptr},
      {"Greeter", "ShowPowerButtons", GreeterKey::Bool, 0, 0, nullptr},
      {"Appearance", "Theme", GreeterKey::Choice, 0, 0, "light|dark"},
      {"Appearance", "Accent", GreeterKey::Color, 0, 0, nullptr},
      {"Appearance", "FontSize", GreeterKey::Int, 8, 32, nullptr},
      {"Appearance", "Scheme", GreeterKey::Name, 0, 0, nullptr},
      {"General", "IconTheme", GreeterKey::Name, 0, 0, nullptr},
      {"Cursor", "Theme", GreeterKey::Name, 0, 0, nullptr},
      {"Cursor", "Size", GreeterKey::Int, 16, 96, nullptr},
  };
  return k;
}

static bool name_ok(const std::string &v, bool allow_list) {
  if (v.size() > 200) return false;
  for (char c : v) {
    bool ok = isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '+' ||
              (allow_list && (c == ',' || c == ' '));
    if (!ok) return false;
  }
  return true;
}

static bool path_ok(const std::string &v) {
  // the greeter user must be able to read it: system locations only
  if (v.empty()) return true;
  if (v.size() > 400 || v[0] != '/' || v.find("..") != std::string::npos) return false;
  for (char c : v)
    if ((unsigned char)c < 32 || c == '\\') return false;
  return starts_with(v, "/usr/share/") || starts_with(v, "/usr/local/share/") || starts_with(v, "/etc/fldesktop/");
}

bool validate_greeter_config(const Ini &in, Ini &out, std::string &err) {
  for (auto &k : greeter_keys()) {
    if (!in.has(k.group, k.key)) continue;
    std::string v = trim(in.get(k.group, k.key));
    bool ok = true;
    switch (k.type) {
      case GreeterKey::Bool: ok = v == "true" || v == "false"; break;
      case GreeterKey::Int: {
        char *end = nullptr;
        long n = strtol(v.c_str(), &end, 10);
        ok = !v.empty() && end && !*end && n >= k.min && n <= k.max;
        break;
      }
      case GreeterKey::Choice: ok = false;
        for (auto &c : split(k.choices, '|'))
          if (v == c) ok = true;
        break;
      case GreeterKey::Color:
        ok = v.size() == 7 && v[0] == '#';
        for (size_t i = 1; ok && i < v.size(); i++) ok = isxdigit((unsigned char)v[i]);
        break;
      case GreeterKey::Name: ok = name_ok(v, false); break;
      case GreeterKey::Names: ok = name_ok(v, true); break;
      case GreeterKey::Path: ok = path_ok(v); break;
    }
    if (!ok) {
      err = std::string("invalid value for ") + k.group + "/" + k.key + ": \"" + v + "\"";
      return false;
    }
    out.set(k.group, k.key, v);
  }
  return true;
}

std::string image_type(const std::string &h) {
  if (h.size() >= 8 && h.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return "png";
  if (h.size() >= 3 && (unsigned char)h[0] == 0xff && (unsigned char)h[1] == 0xd8 && (unsigned char)h[2] == 0xff)
    return "jpg";
  // SVG: an XML document whose first element is <svg
  std::string l = lower(h.substr(0, 4096));
  size_t s = l.find("<svg");
  if (s != std::string::npos && l.find("<!entity") == std::string::npos && l.find("<script") == std::string::npos)
    return "svg";
  return "";
}

}  // namespace fld
