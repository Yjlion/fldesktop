// Calls back (in the FLTK thread) when a file changes. The directory is
// watched, so files replaced by rename() (fld::write_file) are noticed.
#pragma once
#include <functional>
#include <set>
#include <string>
#include <vector>

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

// Calls back when the contents of any of these directories change, also
// for directories that don't exist yet (~/.local/share/flatpak/exports/...
// appears with the first Flatpak): their nearest existing parent is watched
// until they appear. Directories removed and created again are followed.
class DirWatcher {
public:
  DirWatcher(std::vector<std::string> dirs, std::function<void()> cb, double delay = 0.3);
  ~DirWatcher();
  DirWatcher(const DirWatcher &) = delete;
  DirWatcher &operator=(const DirWatcher &) = delete;
  // the watch descriptors' paths (for tests)
  std::vector<std::string> watched() const;
  // processes pending events now (tests; normally FLTK's loop does it)
  void poll();

private:
  int fd_ = -1;
  std::vector<std::string> dirs_;
  std::set<int> target_wds_;
  std::vector<std::pair<int, std::string>> wds_;
  std::function<void()> cb_;
  double delay_;
  bool arm(); // true if a target directory exists that wasn't watched before
  static void fd_cb(int fd, void *data);
  static void fire(void *data);
};

} // namespace fld
