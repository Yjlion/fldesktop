#include "settings_cli.h"

#include <cstdio>
#include <cstring>
#include <sstream>

#include "fld/ini.h"
#include "fld/util.h"

namespace settings_cli {

bool parse_key(const std::string &spec, const std::string &config_dir, Key &out, std::string &err) {
  size_t a = spec.find('.'), b = spec.rfind('.');
  if (a == std::string::npos || a == b || a == 0 || b + 1 >= spec.size() || b == a + 1) {
    err = "expected FILE.SECTION.KEY (e.g. wm.behaviour.workspaces): " + spec;
    return false;
  }
  std::string file = spec.substr(0, a);
  for (char c : file)
    if (!(isalnum((unsigned char)c) || c == '-' || c == '_')) {
      err = "not a settings file name: " + file;
      return false;
    }
  out.file = fld::path_join(config_dir, file + ".ini");
  out.section = spec.substr(a + 1, b - a - 1);
  out.key = spec.substr(b + 1);
  return true;
}

bool safe_members(const std::string &listing, std::string &err) {
  std::istringstream in(listing);
  std::string m;
  int n = 0;
  while (std::getline(in, m)) {
    if (m.empty()) continue;
    n++;
    bool under = m == "fldesktop" || m == "fldesktop/" || fld::starts_with(m, "fldesktop/");
    if (!under || m.find("..") != std::string::npos || m[0] == '/' || m.find('\\') != std::string::npos) {
      err = "the archive has a file outside fldesktop/: " + m;
      return false;
    }
  }
  if (!n) {
    err = "the archive is empty";
    return false;
  }
  return true;
}

static int fail(const std::string &m) {
  fprintf(stderr, "fl-settings: %s\n", m.c_str());
  return 1;
}

int run(int argc, char **argv, const std::string &config_dir) {
  if (argc < 1) return -1;
  std::string cmd = argv[0];
  if (cmd != "--get" && cmd != "--set" && cmd != "--unset" && cmd != "--export" && cmd != "--import") return -1;
  if (argc != 2) return fail("usage: fl-settings " + cmd + (cmd == "--set" ? " FILE.SECTION.KEY=VALUE" :
                             cmd == "--export" || cmd == "--import" ? " ARCHIVE.tar.gz" : " FILE.SECTION.KEY"));
  std::string arg = argv[1], err;
  std::string parent = fld::dirname(config_dir), leaf = fld::basename(config_dir);
  if (cmd == "--export") {
    std::string out;
    if (fld::run_capture_input({"tar", "-czf", arg, "-C", parent, leaf}, "", out, true) != 0)
      return fail("tar failed: " + fld::trim(out));
    return 0;
  }
  if (cmd == "--import") {
    std::string list;
    if (fld::run_capture_input({"tar", "-tzf", arg}, "", list, false) != 0) return fail("cannot read " + arg);
    if (!safe_members(list, err)) return fail(err);
    fld::mkdir_p(parent);
    std::string out;
    if (fld::run_capture_input({"tar", "-xzf", arg, "-C", parent, "--no-same-owner", "--no-same-permissions",
                                "--", "fldesktop"}, "", out, true) != 0)
      return fail("tar failed: " + fld::trim(out));
    fld::reload_wm();
    return 0;
  }
  std::string spec = arg, value;
  if (cmd == "--set") {
    size_t eq = arg.find('=');
    if (eq == std::string::npos) return fail("expected FILE.SECTION.KEY=VALUE: " + arg);
    spec = arg.substr(0, eq);
    value = arg.substr(eq + 1);
  }
  Key k;
  if (!parse_key(spec, config_dir, k, err)) return fail(err);
  fld::Ini ini;
  ini.load(k.file);
  if (cmd == "--get") {
    if (!ini.has(k.section, k.key)) return 1; // not set: the program's default
    printf("%s\n", ini.get(k.section, k.key).c_str());
    return 0;
  }
  if (cmd == "--set") {
    if (value.find('\n') != std::string::npos) return fail("a value must be on one line");
    ini.set(k.section, k.key, value);
  } else {
    ini.remove(k.section, k.key);
  }
  fld::mkdir_p(config_dir);
  if (!ini.save(k.file)) return fail("cannot write " + k.file);
  if (fld::basename(k.file) == "wm.ini") fld::reload_wm();
  return 0;
}

} // namespace settings_cli
