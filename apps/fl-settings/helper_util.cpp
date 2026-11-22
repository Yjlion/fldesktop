#include "helper_util.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fld {

int helper_fail(const std::string &m) {
  printf("%s\n", m.c_str());
  fflush(stdout);
  return 1;
}

uid_t helper_caller_uid() {
  const char *uid_s = getenv("PKEXEC_UID");
  return uid_s && *uid_s ? (uid_t)strtoul(uid_s, nullptr, 10) : getuid();
}

bool read_user_file(const char *path, uid_t uid, size_t max, std::string &out, std::string &err) {
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
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(fd);
  out.resize(got);
  return true;
}

bool write_atomic(const std::string &path, const std::string &data, mode_t mode) {
  std::string tmp = path + ".new";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, mode);
  if (fd < 0) return false;
  bool ok = write(fd, data.data(), data.size()) == (ssize_t)data.size();
  ok = fchmod(fd, mode) == 0 && ok;
  ok = fsync(fd) == 0 && ok;
  close(fd);
  if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
    unlink(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace fld
