#include "stash.h"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "fld/util.h"

namespace stash {

std::string socket_path() {
  const char *rt = getenv("XDG_RUNTIME_DIR");
  std::string dir = rt && *rt ? rt : "/run/user/" + std::to_string(getuid());
  struct stat st;
  if (stat(dir.c_str(), &st) != 0 || st.st_uid != getuid()) return "";
  return dir + "/fldesktop/keyring-stash";
}

static bool peer_is_me(int fd) {
  struct ucred cr;
  socklen_t len = sizeof cr;
  return getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) == 0 && cr.uid == getuid();
}

bool serve(const crypto::Bytes &key, int timeout_s) {
  std::string path = socket_path();
  if (path.empty()) return false;
  std::string dir = fld::dirname(path);
  mkdir(dir.c_str(), 0700);
  struct stat st;
  if (lstat(dir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077)) return false;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  if (path.size() >= sizeof a.sun_path) return false;
  strcpy(a.sun_path, path.c_str());
  unlink(path.c_str());
  mode_t old = umask(077);
  bool ok = bind(fd, (sockaddr *)&a, sizeof a) == 0 && listen(fd, 4) == 0;
  umask(old);
  if (!ok) {
    close(fd);
    return false;
  }
  time_t end = time(nullptr) + timeout_s;
  bool given = false;
  while (!given && time(nullptr) < end) {
    pollfd p{fd, POLLIN, 0};
    int r = poll(&p, 1, (int)(end - time(nullptr)) * 1000);
    if (r <= 0) continue;
    int c = accept4(fd, nullptr, nullptr, SOCK_CLOEXEC);
    if (c < 0) continue;
    if (peer_is_me(c)) {
      size_t off = 0;
      while (off < key.size()) {
        ssize_t n = write(c, key.data() + off, key.size() - off);
        if (n <= 0) break;
        off += (size_t)n;
      }
      given = off == key.size();
    }
    close(c);
  }
  close(fd);
  unlink(path.c_str());
  return given;
}

bool fetch(crypto::Bytes &key) {
  std::string path = socket_path();
  if (path.empty()) return false;
  int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) return false;
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
  if (connect(fd, (sockaddr *)&a, sizeof a) != 0 || !peer_is_me(fd)) {
    close(fd);
    return false;
  }
  key.assign(32, '\0');
  size_t off = 0;
  while (off < 32) {
    ssize_t n = read(fd, &key[off], 32 - off);
    if (n <= 0) break;
    off += (size_t)n;
  }
  close(fd);
  if (off != 32) {
    crypto::wipe(key);
    return false;
  }
  return true;
}

} // namespace stash
