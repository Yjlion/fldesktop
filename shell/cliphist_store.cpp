// fl-cliphist store (see cliphist_store.h)
#include "cliphist_store.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace cliphist {

namespace {

bool mkdirs(const std::string &path) {
  std::string p;
  std::stringstream ss(path);
  std::string part;
  if (!path.empty() && path[0] == '/') p = "/";
  while (std::getline(ss, part, '/')) {
    if (part.empty()) continue;
    p += part + "/";
    mkdir(p.c_str(), 0700);
  }
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool read_all(const std::string &path, std::string &out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

bool write_atomic(const std::string &path, const std::string &data) {
  std::string tmp = path + ".tmp";
  int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return false;
  size_t done = 0;
  while (done < data.size()) {
    ssize_t n = write(fd, data.data() + done, data.size() - done);
    if (n <= 0) {
      close(fd);
      unlink(tmp.c_str());
      return false;
    }
    done += (size_t)n;
  }
  close(fd);
  return rename(tmp.c_str(), path.c_str()) == 0;
}

std::string one_line(const std::string &s) {
  std::string o;
  for (char c : s) o += (c == '\t' || c == '\n' || c == '\r') ? ' ' : c;
  return o;
}

} // namespace

std::string Store::default_dir() {
  const char *st = getenv("XDG_STATE_HOME");
  std::string base = st && *st ? st : std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.local/state";
  return base + "/fldesktop/clipboard";
}

Store::Store(const std::string &dir) : dir_(dir) { mkdirs(dir_); }

int Store::lock() const {
  int fd = open((dir_ + "/lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
  if (fd >= 0) flock(fd, LOCK_EX);
  return fd;
}

void Store::unlock(int fd) const {
  if (fd >= 0) {
    flock(fd, LOCK_UN);
    close(fd);
  }
}

std::vector<Entry> Store::read_index() const {
  std::vector<Entry> out;
  std::string text;
  if (!read_all(dir_ + "/index", text)) return out;
  std::stringstream ss(text);
  std::string line;
  while (std::getline(ss, line)) {
    std::stringstream ls(line);
    std::string id, pinned, t, mime, size, preview;
    if (!std::getline(ls, id, '\t') || !std::getline(ls, pinned, '\t') || !std::getline(ls, t, '\t') ||
        !std::getline(ls, mime, '\t') || !std::getline(ls, size, '\t'))
      continue;
    std::getline(ls, preview);
    Entry e;
    e.id = strtoull(id.c_str(), nullptr, 10);
    if (!e.id) continue;
    e.pinned = pinned == "1";
    e.time = (time_t)strtoll(t.c_str(), nullptr, 10);
    e.mime = mime;
    e.size = (size_t)strtoull(size.c_str(), nullptr, 10);
    e.preview = preview;
    out.push_back(e);
  }
  return out;
}

bool Store::write_index(const std::vector<Entry> &entries) const {
  std::string text;
  for (const Entry &e : entries)
    text += std::to_string(e.id) + "\t" + (e.pinned ? "1" : "0") + "\t" + std::to_string((long long)e.time) + "\t" +
            one_line(e.mime) + "\t" + std::to_string(e.size) + "\t" + one_line(e.preview) + "\n";
  return write_atomic(dir_ + "/index", text);
}

std::vector<Entry> Store::list() const {
  int fd = lock();
  std::vector<Entry> e = read_index();
  unlock(fd);
  return e;
}

uint64_t Store::add(const std::string &mime, const std::string &data, size_t max) {
  if (data.empty()) return 0;
  int fd = lock();
  std::vector<Entry> entries = read_index();
  // the same data again: move it to the top
  for (size_t i = 0; i < entries.size(); i++) {
    Entry &e = entries[i];
    if (e.mime != mime || e.size != data.size()) continue;
    std::string old;
    if (!read_all(dir_ + "/" + std::to_string(e.id), old) || old != data) continue;
    Entry moved = e;
    moved.time = time(nullptr);
    entries.erase(entries.begin() + (long)i);
    entries.insert(entries.begin(), moved);
    write_index(entries);
    unlock(fd);
    return moved.id;
  }
  Entry n;
  for (const Entry &e : entries) n.id = std::max(n.id, e.id);
  n.id++;
  n.time = time(nullptr);
  n.mime = mime;
  n.size = data.size();
  int w, h;
  if (n.is_image()) n.preview = png_size(data, &w, &h) ? "Image " + std::to_string(w) + "×" + std::to_string(h) : "Image";
  else n.preview = text_preview(data);
  if (!write_atomic(dir_ + "/" + std::to_string(n.id), data)) {
    unlock(fd);
    return 0;
  }
  entries.insert(entries.begin(), n);
  // drop the oldest unpinned entries beyond max
  size_t unpinned = 0;
  std::vector<Entry> kept;
  for (const Entry &e : entries) {
    if (!e.pinned && ++unpinned > max) {
      unlink((dir_ + "/" + std::to_string(e.id)).c_str());
      continue;
    }
    kept.push_back(e);
  }
  write_index(kept);
  unlock(fd);
  return n.id;
}

bool Store::data(uint64_t id, std::string &mime, std::string &out) const {
  int fd = lock();
  bool ok = false;
  for (const Entry &e : read_index())
    if (e.id == id) {
      mime = e.mime;
      ok = read_all(dir_ + "/" + std::to_string(id), out);
    }
  unlock(fd);
  return ok;
}

bool Store::set_pinned(uint64_t id, bool pinned) {
  int fd = lock();
  std::vector<Entry> entries = read_index();
  bool found = false;
  for (Entry &e : entries)
    if (e.id == id) {
      e.pinned = pinned;
      found = true;
    }
  if (found) write_index(entries);
  unlock(fd);
  return found;
}

bool Store::remove(uint64_t id) {
  int fd = lock();
  std::vector<Entry> entries = read_index();
  size_t before = entries.size();
  entries.erase(std::remove_if(entries.begin(), entries.end(), [id](const Entry &e) { return e.id == id; }),
                entries.end());
  bool found = entries.size() != before;
  if (found) {
    unlink((dir_ + "/" + std::to_string(id)).c_str());
    write_index(entries);
  }
  unlock(fd);
  return found;
}

void Store::clear(bool keep_pinned) {
  int fd = lock();
  std::vector<Entry> kept;
  for (const Entry &e : read_index()) {
    if (keep_pinned && e.pinned) kept.push_back(e);
    else unlink((dir_ + "/" + std::to_string(e.id)).c_str());
  }
  write_index(kept);
  unlock(fd);
}

std::string text_preview(const std::string &text, size_t max_chars) {
  std::string out;
  size_t chars = 0;
  bool space = false;
  size_t i = 0;
  for (; i < text.size(); i++) {
    unsigned char c = (unsigned char)text[i];
    // stop at max_chars, but not inside a UTF-8 character
    if (chars >= max_chars && (c & 0xc0) != 0x80) break;
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (!out.empty()) space = true;
      continue;
    }
    if (c < 0x20) continue;
    if (space) {
      out += ' ';
      chars++;
      space = false;
    }
    out += (char)c;
    if ((c & 0xc0) != 0x80) chars++; // count UTF-8 characters, not bytes
  }
  if (i < text.size()) out += "…";
  return out;
}

bool png_size(const std::string &d, int *w, int *h) {
  static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (d.size() < 24 || d.compare(0, 8, std::string((const char *)sig, 8)) != 0 || d.compare(12, 4, "IHDR") != 0)
    return false;
  auto be32 = [&](size_t o) {
    return (int)(((unsigned char)d[o] << 24) | ((unsigned char)d[o + 1] << 16) | ((unsigned char)d[o + 2] << 8) |
                 (unsigned char)d[o + 3]);
  };
  *w = be32(16);
  *h = be32(20);
  return *w > 0 && *h > 0;
}

std::string pick_mime(const std::vector<std::string> &offered) {
  auto has = [&](const char *m) { return std::find(offered.begin(), offered.end(), m) != offered.end(); };
  if (has("x-kde-passwordManagerHint")) return "";
  for (const char *m : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING", "image/png"})
    if (has(m)) return m;
  return "";
}

} // namespace cliphist
