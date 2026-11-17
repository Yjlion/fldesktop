// Calls back (in the FLTK thread) when a file changes. The directory is
// watched, so files replaced by rename() (fld::write_file) are noticed.
#pragma once
#include <functional>
#include <string>

namespace fld {

class ConfigWatcher {
public:
  ConfigWatcher(const std::string &path, std::function<void()> cb, double delay = 0.3);
  ~ConfigWatcher();
  ConfigWatcher(const ConfigWatcher &) = delete;
  ConfigWatcher &operator=(const ConfigWatcher &) = delete;

private:
  int fd_ = -1;
  std::string name_;
  std::function<void()> cb_;
  double delay_;
  static void fd_cb(int fd, void *data);
  static void fire(void *data);
};

} // namespace fld
