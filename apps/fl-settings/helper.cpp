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

static int fail(const std::string &m) {
  printf("%s\n", m.c_str());  // stdout: Settings shows it
  return 1;
}

// reads a regular file owned by `uid` without following symlinks
static bool read_user_file(const char *path, uid_t uid, size_t max, std::string &out, std::string &err) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    err = std::string(path) + ": " + strerror(errno);
    return false;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != uid || (size_t)st.st_size > max) {
    close(fd);
    err = std::string(path) + ": not a regular file of the calling user, or too large";
    return false;
  }
  out.resize((size_t)st.st_size);
  size_t got = 0;
  while (got < out.size()) {
    ssize_t n = read(fd, &out[got], out.size() - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(fd);
  out.resize(got);
  return true;
}

static bool write_atomic(const std::string &path, const std::string &data) {
  std::string tmp = path + ".new";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  bool ok = write(fd, data.data(), data.size()) == (ssize_t)data.size();
  ok = fchmod(fd, 0644) == 0 && ok;
  ok = fsync(fd) == 0 && ok;
  close(fd);
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc < 3 || strcmp(argv[1], "greeter-ini") != 0)
    return fail("usage: fl-settings-helper greeter-ini <new.ini> [<background image>]");
  if (geteuid() != 0) return fail("must run as root (through pkexec)");
  const char *uid_s = getenv("PKEXEC_UID");
  uid_t uid = uid_s ? (uid_t)atoi(uid_s) : getuid();

  std::string text, err;
  if (!read_user_file(argv[2], uid, 64 * 1024, text, err)) return fail(err);
  fld::Ini in;
  if (!in.parse(text)) return fail("cannot read the new settings");

  mkdir("/etc/fldesktop", 0755);
  fld::Ini out;
  out.load(fld::greeter_ini_path());  // keeps other keys and comments
  if (!fld::validate_greeter_config(in, out, err)) return fail(err);

  if (argc > 3) {
    std::string img;
    if (!read_user_file(argv[3], uid, 32u << 20, img, err)) return fail(err);
    std::string ext = fld::image_type(img);
    if (ext.empty()) return fail("the background is not a PNG, JPEG or SVG image");
    for (const char *e : {"png", "jpg", "svg"})
      if (ext != e) unlink((std::string("/etc/fldesktop/greeter-background.") + e).c_str());
    std::string dest = "/etc/fldesktop/greeter-background." + ext;
    if (!write_atomic(dest, img)) return fail("cannot write " + dest);
    out.set("Greeter", "Background", dest);
  }
  std::string ini_path = fld::greeter_ini_path();
  if (!write_atomic(ini_path, out.to_string())) return fail("cannot write " + ini_path);
  return 0;
}
