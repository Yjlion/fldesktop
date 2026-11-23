#include "welcome_util.h"

#include <algorithm>

#include "fld/util.h"

namespace welcome {

const std::vector<std::pair<std::string, std::string>> &accents() {
  static const std::vector<std::pair<std::string, std::string>> a = {
      {"Blue", "#3584e4"},   {"Teal", "#2190a4"},  {"Green", "#3a944a"}, {"Yellow", "#c88800"},
      {"Orange", "#ed5b00"}, {"Red", "#e62d42"},   {"Pink", "#d56199"},  {"Purple", "#9141ac"},
      {"Slate", "#6f8396"}};
  return a;
}

std::vector<std::string> wallpapers(const std::vector<std::string> &dirs) {
  std::vector<std::string> out;
  for (const std::string &d : dirs)
    for (const std::string &n : fld::list_dir(d)) {
      std::string e = fld::extension(n);
      if (e == "png" || e == "jpg" || e == "jpeg" || e == "svg") out.push_back(d + "/" + n);
    }
  std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) {
    return fld::basename(a) < fld::basename(b);
  });
  return out;
}

std::string done_marker(const std::string &config_dir) { return config_dir + "/welcome-done"; }

void layout_code(const std::string &code, std::string &layout, std::string &variant) {
  size_t p = code.find('(');
  if (p == std::string::npos) {
    layout = fld::trim(code);
    variant.clear();
    return;
  }
  layout = fld::trim(code.substr(0, p));
  size_t e = code.find(')', p);
  variant = code.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1);
}

std::vector<std::string> flathub_command() {
  return {"flatpak", "remote-add", "--user", "--if-not-exists", "flathub",
          "https://dl.flathub.org/repo/flathub.flatpakrepo"};
}

std::string layout_from_locale(const std::string &lang) {
  // "pt_BR.UTF-8" -> pt / br; a few where the country decides
  std::string l = lang.substr(0, lang.find('.'));
  std::string language = fld::lower(l.substr(0, l.find('_')));
  std::string country = l.find('_') == std::string::npos ? "" : fld::lower(l.substr(l.find('_') + 1));
  if (language.empty() || language == "c" || language == "posix") return "us";
  if (language == "en") return country == "gb" ? "gb" : "us";
  if (language == "pt") return country == "br" ? "br" : "pt";
  if (language == "de" && country == "ch") return "ch";
  if (language == "fr" && country == "ca") return "ca";
  if (language == "fr" && country == "ch") return "ch(fr)";
  if (language == "fr" && country == "be") return "be";
  if (language == "nl" && country == "be") return "be";
  static const std::pair<const char *, const char *> map[] = {
      {"da", "dk"}, {"sv", "se"}, {"nb", "no"}, {"nn", "no"}, {"et", "ee"}, {"cs", "cz"}, {"el", "gr"},
      {"uk", "ua"}, {"ja", "jp"}, {"ko", "kr"}, {"sl", "si"}, {"sr", "rs"}, {"he", "il"}, {"fa", "ir"},
      {"hi", "in"}, {"zh", "cn"}, {"vi", "vn"}, {"ka", "ge"}, {"sq", "al"}, {"ca", "es(cat)"}};
  for (auto &m : map)
    if (language == m.first) return m.second;
  return language;
}

} // namespace welcome
