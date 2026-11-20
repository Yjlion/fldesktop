#include "locale_env.h"

#include <clocale>
#include <cstring>
#include <ctime>
#include <langinfo.h>
#include <locale.h>
#include <monetary.h>
#include <set>

#include "fld/util.h"

namespace locale_env {

std::string normalize(const std::string &l) {
  size_t dot = l.find('.');
  if (dot == std::string::npos) return l;
  std::string cs = fld::lower(l.substr(dot + 1));
  std::string mod;
  size_t at = cs.find('@');
  if (at != std::string::npos) { mod = cs.substr(at); cs = cs.substr(0, at); }
  if (cs == "utf8" || cs == "utf-8") cs = "UTF-8";
  return l.substr(0, dot) + "." + cs + mod;
}

static std::string describe(const std::string &name) {
  locale_t loc = newlocale(LC_ALL_MASK, name.c_str(), (locale_t)0);
  if (!loc) return name;
  std::string lang = nl_langinfo_l(_NL_IDENTIFICATION_LANGUAGE, loc);
  std::string terr = nl_langinfo_l(_NL_IDENTIFICATION_TERRITORY, loc);
  freelocale(loc);
  if (lang.empty()) return name;
  return terr.empty() || terr == "ISO" ? lang : lang + " (" + terr + ")";
}

std::vector<LocaleInfo> installed_locales() {
  std::string out;
  fld::run_capture({"locale", "-a"}, out);
  std::set<std::string> seen;
  std::vector<LocaleInfo> v;
  for (auto &l : fld::split(out, '\n')) {
    std::string n = fld::trim(l);
    std::string low = fld::lower(n);
    if (low.find("utf") == std::string::npos) continue; // only UTF-8 locales
    n = normalize(n);
    if (!seen.insert(n).second) continue;
    v.push_back({n, describe(n)});
  }
  if (seen.find("C.UTF-8") == seen.end()) v.push_back({"C.UTF-8", "Unspecified (C)"});
  return v;
}

bool EnvFile::load(const std::string &path) {
  std::string t;
  if (!fld::read_file(path, t)) return false;
  parse(t);
  return true;
}

void EnvFile::parse(const std::string &text) {
  lines_ = fld::split(text, '\n', true);
  while (!lines_.empty() && lines_.back().empty()) lines_.pop_back();
}

static std::string key_of(const std::string &line) {
  std::string l = fld::trim(line);
  if (fld::starts_with(l, "export ")) l = fld::trim(l.substr(7));
  if (l.empty() || l[0] == '#') return "";
  size_t eq = l.find('=');
  return eq == std::string::npos ? "" : l.substr(0, eq);
}

std::string EnvFile::get(const std::string &key) const {
  for (auto &line : lines_) {
    if (key_of(line) != key) continue;
    std::string v = line.substr(line.find('=') + 1);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    return v;
  }
  return "";
}

void EnvFile::set(const std::string &key, const std::string &value) {
  std::string line = key + "=" + value;
  for (auto it = lines_.begin(); it != lines_.end(); ++it) {
    if (key_of(*it) != key) continue;
    if (value.empty()) lines_.erase(it);
    else *it = line;
    return;
  }
  if (!value.empty()) lines_.push_back(line);
}

std::string EnvFile::text() const {
  std::string s;
  for (auto &l : lines_) s += l + "\n";
  return s;
}

bool EnvFile::save(const std::string &path) const {
  fld::mkdir_p(fld::dirname(path));
  return fld::write_file(path, text());
}

std::string preview(const std::string &locale) {
  locale_t loc = newlocale(LC_ALL_MASK, locale.c_str(), (locale_t)0);
  if (!loc) return "";
  char date[128], num[64], money[64];
  time_t now = time(nullptr);
  struct tm tm;
  localtime_r(&now, &tm);
  strftime_l(date, sizeof date, "%c", &tm, loc);
  locale_t old = uselocale(loc);
  snprintf(num, sizeof num, "%'.2f", 1234567.89);
  if (strfmon_l(money, sizeof money, loc, "%n", 1234.56) < 0) money[0] = 0;
  uselocale(old);
  freelocale(loc);
  return std::string(date) + "\n" + num + "\n" + money;
}


std::vector<std::pair<std::string, std::string>> input_method_env(const std::string &im) {
  if (im == "fcitx5")
    return {{"XMODIFIERS", "@im=fcitx"}, {"QT_IM_MODULE", "fcitx"}, {"SDL_IM_MODULE", "fcitx"}, {"GTK_IM_MODULE", ""}};
  if (im == "ibus")
    return {{"XMODIFIERS", "@im=ibus"}, {"QT_IM_MODULE", "ibus"}, {"SDL_IM_MODULE", "ibus"}, {"GTK_IM_MODULE", "ibus"}};
  return {{"XMODIFIERS", ""}, {"QT_IM_MODULE", ""}, {"SDL_IM_MODULE", ""}, {"GTK_IM_MODULE", ""}};
}

std::string input_method_from_env(const EnvFile &env) {
  std::string x = env.get("XMODIFIERS");
  if (x == "@im=fcitx") return "fcitx5";
  if (x == "@im=ibus") return "ibus";
  return "";
}

void set_input_method(EnvFile &env, const std::string &im) {
  for (const auto &kv : input_method_env(im)) env.set(kv.first, kv.second);
}

std::string input_method_command(const std::string &im) {
  if (im == "fcitx5") return "fcitx5 -d --replace";
  if (im == "ibus") return "ibus-daemon -drx";
  return "";
}

} // namespace locale_env
