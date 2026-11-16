#include "fld/trash.h"
#include "fld/ini.h"
#include "fld/util.h"

#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace fld {

static void set_err(std::string *e, const std::string &what) {
  if (e) *e = what + ": " + strerror(errno);
}

std::string trash_dir() {
  std::string t = data_home() + "/Trash";
  mkdir_p(t + "/files", 0700);
  mkdir_p(t + "/info", 0700);
  return t;
}

bool trash(const std::string &path_in, std::string *error) {
  std::string path = abs_path(path_in);
  std::string t = trash_dir();
  std::string base = basename(path);
  // reserve a unique info file atomically
  std::string name = base, info;
  for (int i = 1;; i++) {
    info = t + "/info/" + name + ".trashinfo";
    int fd = open(info.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd >= 0) {
      std::string body = "[Trash Info]\nPath=" + uri_encode_path(path) +
                         "\nDeletionDate=" + format_time(time(nullptr), "%Y-%m-%dT%H:%M:%S") + "\n";
      bool ok = write(fd, body.data(), body.size()) == (ssize_t)body.size();
      close(fd);
      if (!ok) { set_err(error, "write trashinfo"); unlink(info.c_str()); return false; }
      break;
    }
    if (errno != EEXIST) { set_err(error, "create trashinfo"); return false; }
    name = base + "." + std::to_string(i);
    if (i > 100000) return false;
  }
  std::string dest = t + "/files/" + name;
  if (!move_path(path, dest, error)) {
    unlink(info.c_str());
    return false;
  }
  return true;
}

std::vector<TrashItem> trash_list() {
  std::vector<TrashItem> v;
  std::string t = trash_dir();
  for (auto &n : list_dir(t + "/files", true)) {
    TrashItem it;
    it.name = n;
    Ini ini;
    if (ini.load(t + "/info/" + n + ".trashinfo")) {
      it.original_path = uri_decode(ini.get("Trash Info", "Path"));
      struct tm tm{};
      std::string d = ini.get("Trash Info", "DeletionDate");
      if (strptime(d.c_str(), "%Y-%m-%dT%H:%M:%S", &tm)) {
        tm.tm_isdst = -1;
        it.deletion_date = mktime(&tm);
      }
    }
    v.push_back(it);
  }
  return v;
}

bool trash_restore(const TrashItem &item, std::string *error) {
  std::string t = trash_dir();
  std::string dest = item.original_path.empty() ? home_dir() + "/" + item.name : item.original_path;
  if (file_exists(dest)) dest = unique_name(dirname(dest), basename(dest));
  mkdir_p(dirname(dest));
  if (!move_path(t + "/files/" + item.name, dest, error)) return false;
  unlink((t + "/info/" + item.name + ".trashinfo").c_str());
  return true;
}

bool trash_delete(const TrashItem &item) {
  std::string t = trash_dir();
  bool ok = remove_recursive(t + "/files/" + item.name);
  unlink((t + "/info/" + item.name + ".trashinfo").c_str());
  return ok;
}

bool trash_empty() {
  bool ok = true;
  for (auto &i : trash_list()) ok = trash_delete(i) && ok;
  return ok;
}

bool remove_recursive(const std::string &path) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return errno == ENOENT;
  if (S_ISDIR(st.st_mode)) {
    bool ok = true;
    for (auto &n : list_dir(path, true)) ok = remove_recursive(path + "/" + n) && ok;
    return rmdir(path.c_str()) == 0 && ok;
  }
  return unlink(path.c_str()) == 0;
}

static bool copy_file(const std::string &src, const std::string &dst, const struct stat &st,
                      std::string *error, void (*progress)(unsigned long long, void *), void *data) {
  int in = open(src.c_str(), O_RDONLY);
  if (in < 0) { set_err(error, src); return false; }
  int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, st.st_mode & 07777);
  if (out < 0) { set_err(error, dst); close(in); return false; }
  static thread_local char buf[1 << 17];
  ssize_t n;
  bool ok = true;
  while ((n = read(in, buf, sizeof buf)) > 0) {
    char *p = buf;
    while (n > 0) {
      ssize_t w = write(out, p, n);
      if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
      p += w;
      n -= w;
      if (progress) progress((unsigned long long)w, data);
    }
    if (!ok) break;
  }
  if (n < 0) ok = false;
  if (!ok) set_err(error, dst);
  struct timespec ts[2] = {st.st_atim, st.st_mtim};
  futimens(out, ts);
  close(in);
  if (close(out) != 0) ok = false;
  if (!ok) unlink(dst.c_str());
  return ok;
}

bool copy_recursive(const std::string &src, const std::string &dst, std::string *error,
                    void (*progress)(unsigned long long, void *), void *data) {
  struct stat st;
  if (lstat(src.c_str(), &st) != 0) { set_err(error, src); return false; }
  if (S_ISLNK(st.st_mode)) {
    char target[4096];
    ssize_t n = readlink(src.c_str(), target, sizeof target - 1);
    if (n < 0) { set_err(error, src); return false; }
    target[n] = 0;
    if (symlink(target, dst.c_str()) != 0) { set_err(error, dst); return false; }
    return true;
  }
  if (S_ISDIR(st.st_mode)) {
    // refuse to copy a directory into itself
    std::string as = abs_path(src) + "/", ad = abs_path(dst) + "/";
    if (starts_with(ad, as)) { if (error) *error = "Cannot copy a folder into itself"; return false; }
    if (mkdir(dst.c_str(), (st.st_mode & 07777) | 0700) != 0) { set_err(error, dst); return false; }
    bool ok = true;
    for (auto &n : list_dir(src, true))
      ok = copy_recursive(src + "/" + n, dst + "/" + n, error, progress, data) && ok;
    chmod(dst.c_str(), st.st_mode & 07777);
    return ok;
  }
  if (S_ISREG(st.st_mode)) return copy_file(src, dst, st, error, progress, data);
  if (error) *error = src + ": special file not copied";
  return false;
}

bool move_path(const std::string &src, const std::string &dst, std::string *error) {
  if (rename(src.c_str(), dst.c_str()) == 0) return true;
  if (errno != EXDEV) { set_err(error, src); return false; }
  if (!copy_recursive(src, dst, error)) return false;
  if (!remove_recursive(src)) { if (error) *error = "copied, but could not remove " + src; return false; }
  return true;
}

} // namespace fld
