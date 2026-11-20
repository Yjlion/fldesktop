// fl-cliphist: the clipboard history on disk, shared by the daemon, its
// command line and fl-panel. $XDG_STATE_HOME/fldesktop/clipboard holds an
// index (newest first) and one file per entry; every change happens under a
// lock and is written with a rename, so the three can use it at once.
#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace cliphist {

struct Entry {
  uint64_t id = 0;
  bool pinned = false;
  time_t time = 0;
  std::string mime;    // text/plain;charset=utf-8, image/png, ...
  size_t size = 0;
  std::string preview; // text: the first characters on one line; images: "Image WxH"
  bool is_image() const { return mime.rfind("image/", 0) == 0; }
};

class Store {
public:
  explicit Store(const std::string &dir = default_dir());
  static std::string default_dir();

  std::vector<Entry> list() const;
  // Adds data (newest first). The same data again moves its entry to the top.
  // Unpinned entries beyond `max` are dropped. Returns the entry's id (0 on error).
  uint64_t add(const std::string &mime, const std::string &data, size_t max);
  bool data(uint64_t id, std::string &mime, std::string &out) const;
  bool set_pinned(uint64_t id, bool pinned);
  bool remove(uint64_t id);
  void clear(bool keep_pinned = true);
  const std::string &dir() const { return dir_; }

private:
  std::string dir_;
  std::vector<Entry> read_index() const;
  bool write_index(const std::vector<Entry> &e) const;
  int lock() const; // returns the fd to unlock()
  void unlock(int fd) const;
};

// helpers (unit tested)
std::string text_preview(const std::string &text, size_t max_chars = 120);
// width and height from a PNG header, false if it is not one
bool png_size(const std::string &data, int *w, int *h);
// the mime type to keep from what a program offers: UTF-8 text first, then
// plain text, then PNG; "" for nothing usable, and for password managers
// (x-kde-passwordManagerHint) and sensitive data
std::string pick_mime(const std::vector<std::string> &offered);

} // namespace cliphist
