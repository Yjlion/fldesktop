#include "remotes.h"

#include <cctype>

#include "fld/util.h"

namespace fp {

namespace {
std::string inst_flag(const std::string &installation) { return installation == "user" ? "--user" : "--system"; }
} // namespace

std::string remote_name_for(const std::string &title_or_url) {
  std::string s = title_or_url;
  size_t scheme = s.find("://");
  if (scheme != std::string::npos) { // https://dl.example.org/repo/foo.flatpakrepo -> foo, else the host
    std::string path = s.substr(scheme + 3);
    std::string last = fld::basename(path);
    if (fld::extension(last) == "flatpakrepo") s = last.substr(0, last.rfind('.'));
    else {
      s = path.substr(0, path.find('/'));
      if (fld::starts_with(s, "dl.")) s = s.substr(3);
      if (s.find('.') != std::string::npos) s = s.substr(0, s.rfind('.'));
    }
  } else if (fld::extension(s) == "flatpakrepo") {
    s = fld::basename(s);
    s = s.substr(0, s.rfind('.'));
  }
  std::string r;
  for (char c : s) {
    if (isalnum((unsigned char)c) || c == '_' || c == '.') r += (char)tolower((unsigned char)c);
    else if (!r.empty() && r.back() != '-') r += '-';
  }
  while (!r.empty() && (r.back() == '-' || r.back() == '.')) r.pop_back();
  return r.empty() ? "source" : r;
}

std::vector<std::string> remote_add_cmd(const std::string &installation, const std::string &name,
                                        const std::string &url, const std::string &title, const std::string &gpg_key) {
  std::vector<std::string> c = {"flatpak", "remote-add", inst_flag(installation), "--if-not-exists"};
  if (gpg_key == "-") c.push_back("--no-gpg-verify");
  else if (!gpg_key.empty()) c.push_back("--gpg-import=" + gpg_key);
  if (!title.empty()) c.push_back("--title=" + title);
  bool repofile = fld::extension(url) == "flatpakrepo";
  if (repofile) c.push_back("--from");
  c.push_back(name);
  c.push_back(url);
  return c;
}

std::vector<std::string> remote_modify_cmd(const Remote &r) {
  std::vector<std::string> c = {"flatpak", "remote-modify", inst_flag(r.installation)};
  c.push_back(r.disabled ? "--disable" : "--enable");
  c.push_back("--prio=" + std::to_string(r.prio));
  if (!r.title.empty()) c.push_back("--title=" + r.title);
  c.push_back(r.name);
  return c;
}

std::vector<std::string> remote_delete_cmd(const Remote &r, bool force) {
  std::vector<std::string> c = {"flatpak", "remote-delete", inst_flag(r.installation)};
  if (force) c.push_back("--force");
  c.push_back(r.name);
  return c;
}

} // namespace fp
