// fl-secretd: fldesktop's password store, the Secret Service
// (org.freedesktop.secrets) that programs keep their passwords in, in place
// of GNOME Keyring. Keyrings: ~/.local/share/fldesktop/keyrings.
//
//   fl-secretd                    the service (D-Bus activated, or at login)
//   fl-secretd --login            (pam_fldesktop_keyring) the password on stdin:
//                                 unlocks or makes the "login" keyring and keeps
//                                 its key for the service in the background
//   fl-secretd --change-login     (PAM, password change) "old\nnew" on stdin
//   fl-secretd --ask MODE LABEL   the password dialogs (the service runs it)
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <glib.h>
#include <string>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "crypto.h"
#include "fld/util.h"
#include "prompt.h"
#include "service.h"
#include "stash.h"
#include "store.h"

namespace {

std::string keyrings_dir() { return fld::data_home() + "/fldesktop/keyrings"; }

std::string read_stdin() {
  std::string s;
  char buf[512];
  ssize_t n;
  while ((n = read(0, buf, sizeof buf)) > 0) s.append(buf, (size_t)n);
  memset(buf, 0, sizeof buf);
  return s;
}

std::string self_path(const char *argv0) {
  char b[4096];
  ssize_t n = readlink("/proc/self/exe", b, sizeof b - 1);
  if (n > 0) return std::string(b, (size_t)n);
  return argv0;
}

// no core dumps or ptrace by others of the same user: secrets live in memory
void harden() {
  prctl(PR_SET_DUMPABLE, 0);
  struct rlimit r{0, 0};
  setrlimit(RLIMIT_CORE, &r);
}

int login(bool change) {
  std::string input = read_stdin();
  std::string dir = keyrings_dir(), file = dir + "/login.keyring";
  std::string err;
  store::Collection c;
  if (change) {
    size_t nl = input.find('\n');
    if (nl == std::string::npos || !fld::file_exists(file)) return 0;
    std::string old_pw = input.substr(0, nl), new_pw = input.substr(nl + 1);
    if (!new_pw.empty() && new_pw.back() == '\n') new_pw.pop_back();
    bool ok = store::load(c, file, err) && store::change_password(c, old_pw, new_pw, err);
    crypto::wipe(input);
    crypto::wipe(old_pw);
    crypto::wipe(new_pw);
    if (!ok) fprintf(stderr, "fl-secretd: the login keyring keeps its password: %s\n", err.c_str());
    return ok ? 0 : 1;
  }
  std::string pw = input;
  if (!pw.empty() && pw.back() == '\n') pw.pop_back();
  crypto::wipe(input);
  bool ok;
  if (fld::file_exists(file)) ok = store::load(c, file, err) && store::unlock(c, pw, err);
  else ok = store::create(c, dir, "login", "Login", pw, err);
  crypto::wipe(pw);
  if (!ok) {
    // a password changed elsewhere: the keyring stays locked and asks later
    fprintf(stderr, "fl-secretd: login keyring: %s\n", err.c_str());
    return 0;
  }
  crypto::Bytes key = c.key;
  store::lock(c);
  // the background: PAM must not wait for us
  pid_t pid = fork();
  if (pid != 0) {
    crypto::wipe(key);
    return pid < 0 ? 1 : 0;
  }
  setsid();
  int nul = open("/dev/null", O_RDWR);
  if (nul >= 0) {
    dup2(nul, 0);
    dup2(nul, 1);
    dup2(nul, 2);
    if (nul > 2) close(nul);
  }
  stash::serve(key, 300);
  crypto::wipe(key);
  _exit(0);
}

} // namespace

int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "--ask" && argc >= 4) return ask_main(argv[2], argv[3], argc > 4 ? argv[4] : "");
  if (mode == "-h" || mode == "--help") {
    printf("Usage: fl-secretd [--login | --change-login]\n"
           "The Secret Service (org.freedesktop.secrets) of fldesktop; keyrings in %s\n",
           keyrings_dir().c_str());
    return 0;
  }
  std::string self = self_path(argv[0]); // before harden(): /proc/self is then root's
  harden();
  umask(077);
  if (mode == "--login") return login(false);
  if (mode == "--change-login") return login(true);
  secretd::Options opt;
  opt.dir = keyrings_dir();
  opt.self = self;
  return secretd::run(opt, [] {
    // unlocked at login: the key the PAM module's helper keeps for us
    crypto::Bytes key;
    if (stash::fetch(key)) {
      secretd::reload_collection("login");
      secretd::unlock_with_key("login", key);
      crypto::wipe(key);
    }
  });
}
