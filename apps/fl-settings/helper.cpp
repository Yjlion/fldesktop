// fl-settings-helper: the parts of Settings that need root, run with pkexec.
//
//   fl-settings-helper greeter-ini <new.ini> [<background image>]
//
// Writes /etc/fldesktop/greeter.ini with the validated keys of <new.ini>.
// A background image is copied to /etc/fldesktop/greeter-background.<ext>
// (the greeter cannot read the user's home). Both files must belong to the
// user who ran pkexec.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "fld/ini.h"
#include "fld/util.h"
#include "greeter_config.h"
#include "helper_util.h"

using fld::helper_fail;
static int fail(const std::string &m) { return helper_fail(m); }

int main(int argc, char **argv) {
  if (argc < 3 || strcmp(argv[1], "greeter-ini") != 0)
    return fail("usage: fl-settings-helper greeter-ini <new.ini> [<background image>]");
  if (geteuid() != 0) return fail("must run as root (through pkexec)");
  uid_t uid = fld::helper_caller_uid();

  std::string text, err;
  if (!fld::read_user_file(argv[2], uid, 64 * 1024, text, err)) return fail(err);
  fld::Ini in;
  if (!in.parse(text)) return fail("cannot read the new settings");

  mkdir("/etc/fldesktop", 0755);
  fld::Ini out;
  out.load(fld::greeter_ini_path());  // keeps other keys and comments
  if (!fld::validate_greeter_config(in, out, err)) return fail(err);

  if (argc > 3) {
    std::string img;
    if (!fld::read_user_file(argv[3], uid, 32u << 20, img, err)) return fail(err);
    std::string ext = fld::image_type(img);
    if (ext.empty()) return fail("the background is not a PNG, JPEG or SVG image");
    for (const char *e : {"png", "jpg", "svg"})
      if (ext != e) unlink((std::string("/etc/fldesktop/greeter-background.") + e).c_str());
    std::string dest = "/etc/fldesktop/greeter-background." + ext;
    if (!fld::write_atomic(dest, img)) return fail("cannot write " + dest);
    out.set("Greeter", "Background", dest);
  }
  std::string ini_path = fld::greeter_ini_path();
  if (!fld::write_atomic(ini_path, out.to_string())) return fail("cannot write " + ini_path);
  return 0;
}
