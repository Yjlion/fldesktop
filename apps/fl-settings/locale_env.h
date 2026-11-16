// Language & Region helpers: installed locales and the session environment file
#pragma once
#include <map>
#include <string>
#include <vector>

namespace locale_env {

struct LocaleInfo {
  std::string name;         // e.g. de_DE.UTF-8
  std::string description;  // e.g. "German (Germany)"
};

// UTF-8 locales from `locale -a`, with names from the locale data
std::vector<LocaleInfo> installed_locales();
// Normalizes "de_DE.utf8" -> "de_DE.UTF-8"
std::string normalize(const std::string &locale);

// KEY=value file (~/.config/fldesktop/environment, sourced by fl-session).
// Other lines and comments are kept.
class EnvFile {
public:
  bool load(const std::string &path);
  void parse(const std::string &text);
  std::string get(const std::string &key) const;
  void set(const std::string &key, const std::string &value); // "" removes
  std::string text() const;
  bool save(const std::string &path) const;
private:
  std::vector<std::string> lines_;
};

// Formats a sample date, number and currency amount in a locale.
std::string preview(const std::string &locale);

} // namespace locale_env
