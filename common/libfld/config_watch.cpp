#include "fld/config_watch.h"
#include "fld/util.h"

#include <FL/Fl.H>
#include <sys/inotify.h>
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

} // namespace fld
