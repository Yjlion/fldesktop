// fl-backlight-helper: sets a screen backlight where logind cannot (no
// systemd-logind, elogind without SetBrightness). Run with pkexec:
//
//   fl-backlight-helper <device> <value>
//
// writes <value> to /sys/class/backlight/<device>/brightness, after checking
// that the device exists and the value is within its range.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fld/util.h"

static int fail(const char *m) {
  fprintf(stderr, "fl-backlight-helper: %s\n", m);
  return 1;
}

int main(int argc, char **argv) {
  if (argc != 3) return fail("usage: fl-backlight-helper <device> <value>");
  std::string name = argv[1], value = argv[2];
  if (name.empty() || name[0] == '.' || name.find('/') != std::string::npos) return fail("not a backlight device");
  if (value.empty() || value.size() > 9 || value.find_first_not_of("0123456789") != std::string::npos)
    return fail("not a brightness value");
  std::string dir = "/sys/class/backlight/" + name;
  std::string max_s;
  if (!fld::read_file(dir + "/max_brightness", max_s, 64)) return fail("no such backlight device");
  long max = atol(fld::trim(max_s).c_str()), v = atol(value.c_str());
  if (max <= 0 || v < 0 || v > max) return fail("the value is out of range");
  FILE *f = fopen((dir + "/brightness").c_str(), "w");
  if (!f) return fail("cannot open the brightness file");
  bool ok = fprintf(f, "%ld\n", v) > 0;
  if (fclose(f) != 0 || !ok) return fail("cannot write the brightness");
  return 0;
}
