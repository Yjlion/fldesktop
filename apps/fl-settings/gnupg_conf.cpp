#include "gnupg_conf.h"

#include <sstream>

#include "fld/util.h"

namespace gnupg_conf {

static bool is_pinentry_line(const std::string &line, std::string *value) {
  std::string t = fld::trim(line);
  if (t.compare(0, 16, "pinentry-program") != 0) return false;
  if (t.size() > 16 && t[16] != ' ' && t[16] != '\t') return false;
  if (value) *value = fld::trim(t.substr(16));
  return true;
}

std::string pinentry(const std::string &conf) {
  std::istringstream in(conf);
  std::string line, v, last;
  while (std::getline(in, line))
    if (is_pinentry_line(line, &v)) last = v; // the last one counts
  return last;
}

std::string with_pinentry(const std::string &conf, const std::string &path) {
  std::istringstream in(conf);
  std::string line, out;
  bool done = false;
  while (std::getline(in, line)) {
    if (is_pinentry_line(line, nullptr)) {
      if (!done && !path.empty()) out += "pinentry-program " + path + "\n";
      done = true;
      continue;
    }
    out += line + "\n";
  }
  if (!done && !path.empty()) out += "pinentry-program " + path + "\n";
  return out;
}

std::string conf_path() {
  std::string home = fld::getenv_or("GNUPGHOME", fld::home_dir() + "/.gnupg");
  return home + "/gpg-agent.conf";
}

} // namespace gnupg_conf
