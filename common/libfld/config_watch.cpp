#include "fld/config_watch.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <algorithm>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fld {

ConfigWatcher::ConfigWatcher(const std::string &path, std::function<void()> cb, double delay)
    : name_(basename(path)), cb_(std::move(cb)), delay_(delay) {
  std::string dir = dirname(path);
  mkdir_p(dir);
  fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd_ < 0) return;
  inotify_add_watch(fd_, dir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE);
  Fl::add_fd(fd_, FL_READ, fd_cb, this);
}

ConfigWatcher::~ConfigWatcher() {
  Fl::remove_timeout(fire, this);
  if (fd_ >= 0) {
    Fl::remove_fd(fd_);
    close(fd_);
  }
}

void ConfigWatcher::fd_cb(int fd, void *data) {
  ConfigWatcher *w = static_cast<ConfigWatcher *>(data);
  alignas(inotify_event) char buf[4096];
  bool hit = false;
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    for (char *p = buf; p < buf + n;) {
      inotify_event *ev = (inotify_event *)p;
      if (ev->len && w->name_ == ev->name) hit = true;
      p += sizeof(inotify_event) + ev->len;
    }
  }
  if (hit) {
    Fl::remove_timeout(fire, w);
    Fl::add_timeout(w->delay_, fire, w);
  }
}

void ConfigWatcher::fire(void *data) {
  ConfigWatcher *w = static_cast<ConfigWatcher *>(data);
  if (w->cb_) w->cb_();
}

DirWatcher::DirWatcher(std::vector<std::string> dirs, std::function<void()> cb, double delay)
    : dirs_(std::move(dirs)), cb_(std::move(cb)), delay_(delay) {
  fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (fd_ < 0) return;
  arm();
  Fl::add_fd(fd_, FL_READ, fd_cb, this);
}

DirWatcher::~DirWatcher() {
  Fl::remove_timeout(fire, this);
  if (fd_ >= 0) {
    Fl::remove_fd(fd_);
    close(fd_);
  }
}

static bool is_directory(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool DirWatcher::arm() {
  const uint32_t contents = IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_CLOSE_WRITE |
                            IN_DELETE_SELF | IN_MOVE_SELF | IN_ATTRIB;
  const uint32_t parent = IN_CREATE | IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
  bool appeared = false;
  for (const std::string &d : dirs_) {
    std::string p = d;
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    if (is_directory(p)) {
      int wd = inotify_add_watch(fd_, p.c_str(), contents | IN_ONLYDIR);
      if (wd >= 0 && target_wds_.insert(wd).second) {
        appeared = true;
        wds_.push_back({wd, p});
      }
      continue;
    }
    // the nearest parent that exists, until the directory itself does
    while (p.size() > 1 && !is_directory(p)) p = dirname(p);
    int wd = inotify_add_watch(fd_, p.c_str(), parent);
    if (wd >= 0 && std::none_of(wds_.begin(), wds_.end(), [wd](auto &e) { return e.first == wd; }))
      wds_.push_back({wd, p});
  }
  return appeared;
}

std::vector<std::string> DirWatcher::watched() const {
  std::vector<std::string> r;
  for (auto &e : wds_) r.push_back(e.second);
  return r;
}

void DirWatcher::poll() {
  if (fd_ >= 0) fd_cb(fd_, this);
}

void DirWatcher::fd_cb(int fd, void *data) {
  DirWatcher *w = static_cast<DirWatcher *>(data);
  alignas(inotify_event) char buf[4096];
  bool hit = false, rearm = false;
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    for (char *p = buf; p < buf + n;) {
      inotify_event *ev = (inotify_event *)p;
      if (ev->mask & IN_IGNORED) { // its directory is gone
        w->target_wds_.erase(ev->wd);
        w->wds_.erase(std::remove_if(w->wds_.begin(), w->wds_.end(), [&](auto &e) { return e.first == ev->wd; }),
                      w->wds_.end());
        rearm = hit = true;
      } else if (w->target_wds_.count(ev->wd)) {
        hit = true;
        if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) rearm = true;
      } else {
        rearm = true; // something new in a parent: maybe a path to a target
      }
      p += sizeof(inotify_event) + ev->len;
    }
  }
  if (rearm && w->arm()) hit = true;
  if (hit) {
    Fl::remove_timeout(fire, w);
    if (w->delay_ <= 0) fire(w);
    else Fl::add_timeout(w->delay_, fire, w);
  }
}

void DirWatcher::fire(void *data) {
  DirWatcher *w = static_cast<DirWatcher *>(data);
  if (w->cb_) w->cb_();
}

} // namespace fld
