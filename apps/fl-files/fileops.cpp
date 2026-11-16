#include "fileops.h"

#include <FL/Fl.H>

#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fld/trash.h"
#include "fld/util.h"

unsigned long long tree_size(const std::string &path, std::atomic<bool> *cancel, unsigned long long *files,
                             unsigned long long *dirs) {
  struct stat st;
  if (lstat(path.c_str(), &st) != 0) return 0;
  if (!S_ISDIR(st.st_mode)) {
    if (files) (*files)++;
    return S_ISREG(st.st_mode) ? (unsigned long long)st.st_size : 0;
  }
  if (dirs) (*dirs)++;
  unsigned long long total = 0;
  DIR *d = opendir(path.c_str());
  if (!d) return 0;
  while (struct dirent *e = readdir(d)) {
    if (cancel && *cancel) break;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    total += tree_size(path + "/" + e->d_name, cancel, files, dirs);
  }
  closedir(d);
  return total;
}

FileOp::~FileOp() {
  cancel = true;
  if (worker_.joinable()) worker_.join();
}

void FileOp::start(std::function<void(FileOp *)> on_done) {
  on_done_ = std::move(on_done);
  worker_ = std::thread([this] { run(); });
}

void FileOp::set_current(const std::string &s) {
  std::lock_guard<std::mutex> l(mtx);
  current = s;
}

void FileOp::error(const std::string &e) {
  std::lock_guard<std::mutex> l(mtx);
  errors.push_back(e);
}

void FileOp::awake_done(void *data) {
  FileOp *op = static_cast<FileOp *>(data);
  if (op->worker_.joinable()) op->worker_.join();
  if (op->on_done_) op->on_done_(op);
}

namespace {


void progress_cb(unsigned long long n, void *data) {
  static_cast<FileOp *>(data)->done_bytes += n;
}

std::string dest_for(const std::string &dir, const std::string &src, FileOp::Conflict c, bool &skip) {
  skip = false;
  std::string name = fld::basename(src);
  std::string d = fld::path_join(dir, name);
  struct stat st;
  if (lstat(d.c_str(), &st) != 0) return d;
  if (fld::abs_path(d) == fld::abs_path(src)) return fld::unique_name(dir, name); // duplicate in place
  switch (c) {
    case FileOp::SKIP: skip = true; return d;
    case FileOp::REPLACE: fld::remove_recursive(d); return d;
    default: return fld::unique_name(dir, name);
  }
}

} // namespace

void FileOp::run() {
  if (kind == COPY || kind == MOVE) {
    unsigned long long t = 0;
    for (auto &s : sources) t += tree_size(s, &cancel);
    total_bytes = t;
  }
  for (auto &src : sources) {
    if (cancel) break;
    set_current(fld::basename(src));
    std::string err;
    switch (kind) {
      case COPY: {
        bool skip;
        std::string dst = dest_for(dest_dir, src, conflict, skip);
        if (skip) break;
        if (fld::starts_with(fld::abs_path(dst) + "/", fld::abs_path(src) + "/")) {
          error("Cannot copy " + fld::basename(src) + " into itself");
          break;
        }
        if (!fld::copy_recursive(src, dst, &err, progress_cb, this)) error(err);
        break;
      }
      case MOVE: {
        bool skip;
        if (fld::abs_path(fld::dirname(src)) == fld::abs_path(dest_dir)) break; // already there
        std::string dst = dest_for(dest_dir, src, conflict, skip);
        if (skip) break;
        if (fld::starts_with(fld::abs_path(dst) + "/", fld::abs_path(src) + "/")) {
          error("Cannot move " + fld::basename(src) + " into itself");
          break;
        }
        if (rename(src.c_str(), dst.c_str()) == 0) {
          done_bytes += tree_size(dst);
        } else if (errno == EXDEV) {
          if (!fld::copy_recursive(src, dst, &err, progress_cb, this)) error(err);
          else if (!fld::remove_recursive(src)) error("Copied, but could not remove " + src);
        } else {
          error(fld::basename(src) + ": " + strerror(errno));
        }
        break;
      }
      case DELETE:
        if (!fld::remove_recursive(src)) error("Could not delete " + src + ": " + strerror(errno));
        break;
      case TRASH:
        if (!fld::trash(src, &err)) error(err);
        break;
    }
    done_items++;
  }
  finished = true;
  Fl::awake(awake_done, this);
}
