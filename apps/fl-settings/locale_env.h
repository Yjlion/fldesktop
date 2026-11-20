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

// Input methods: "" (none), "fcitx5" or "ibus". The environment variables
// each one needs; fcitx5 talks to fl-wm through input-method-v2 and to Wayland
// programs through text-input-v3, so GTK needs no module for it.
std::vector<std::pair<std::string, std::string>> input_method_env(const std::string &im);
// which one an environment file sets up ("" when none of ours)
std::string input_method_from_env(const EnvFile &env);
// writes the variables for `im`, removing the other one's
void set_input_method(EnvFile &env, const std::string &im);
// the command that starts it ("" for none)
std::string input_method_command(const std::string &im);

// Formats a sample date, number and currency amount in a locale.
std::string preview(const std::string &locale);

} // namespace locale_env
