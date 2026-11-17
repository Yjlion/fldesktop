#include "search.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace search {

static std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

NameMatcher::NameMatcher(const Query &q) : icase_(!q.match_case) {
  pattern_ = q.name;
  if (pattern_.empty()) { mode_ = ALL; return; }
  if (q.name_regex) {
    mode_ = REGEX;
    try {
      re_ = std::regex(pattern_, std::regex::ECMAScript | (icase_ ? std::regex::icase : std::regex::ECMAScript));
    } catch (const std::regex_error &) {
      valid_ = false;
    }
    return;
  }
  if (pattern_.find_first_of("*?[") != std::string::npos) mode_ = GLOB;
  else {
    mode_ = SUBSTRING;
    if (icase_) pattern_ = lower(pattern_);
  }
}

bool NameMatcher::operator()(const std::string &name) const {
  switch (mode_) {
    case ALL: return true;
    case SUBSTRING: return (icase_ ? lower(name) : name).find(pattern_) != std::string::npos;
    case GLOB: return fnmatch(pattern_.c_str(), name.c_str(), icase_ ? FNM_CASEFOLD : 0) == 0;
    case REGEX: return valid_ && std::regex_search(name, re_);
  }
  return false;
}

bool file_contains(const std::string &path, const std::string &needle_in, bool match_case,
                   unsigned long long max_bytes) {
  if (needle_in.empty()) return true;
  int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (unsigned long long)st.st_size > max_bytes) {
    close(fd);
    return false;
  }
  std::string needle = match_case ? needle_in : lower(needle_in);
  const size_t CHUNK = 64 << 10;
  std::string buf, carry;
  buf.resize(CHUNK);
  bool first = true, found = false;
  ssize_t n;
  while (!found && (n = read(fd, &buf[0], CHUNK)) > 0) {
    std::string chunk(buf.data(), (size_t)n);
    if (first) {
      first = false;
      if (memchr(chunk.data(), 0, std::min<size_t>(chunk.size(), 8192))) break; // binary
    }
    if (!match_case) chunk = lower(chunk);
    std::string hay = carry + chunk;
    if (hay.find(needle) != std::string::npos) found = true;
    // keep the tail so matches across chunk borders are found
    carry = hay.size() >= needle.size() ? hay.substr(hay.size() - (needle.size() - 1)) : hay;
  }
  close(fd);
  return found;
}

Search::~Search() { cancel(); }

void Search::cancel() {
  stop_ = true;
  if (worker_.joinable()) worker_.join();
  running_ = false;
}

bool Search::start(const Query &q) {
  cancel();
  NameMatcher m(q);
  if (!m.valid()) return false;
  stop_ = false;
  truncated_ = false;
  found_ = 0;
  scanned_ = 0;
  {
    std::lock_guard<std::mutex> l(mtx_);
    pending_.clear();
    current_ = q.root;
  }
  running_ = true;
  worker_ = std::thread(&Search::work, this, q);
  return true;
}

size_t Search::take(std::vector<std::string> &out) {
  std::lock_guard<std::mutex> l(mtx_);
  size_t n = pending_.size();
  for (auto &p : pending_) out.push_back(std::move(p));
  pending_.clear();
  return n;
}

std::string Search::current_dir() {
  std::lock_guard<std::mutex> l(mtx_);
  return current_;
}

static bool virtual_fs(const std::string &p) {
  return p == "/proc" || p == "/sys" || p == "/dev" || p == "/run/user" || p == "/run/udev";
}

void Search::work(Query q) {
  NameMatcher match(q);
  std::set<std::pair<dev_t, ino_t>> visited;
  struct Dir {
    std::string path;
    int depth;
  };
  std::vector<Dir> stack;
  std::string root = q.root;
  while (root.size() > 1 && root.back() == '/') root.pop_back();
  struct stat rst;
  if (stat(root.c_str(), &rst) == 0) visited.insert({rst.st_dev, rst.st_ino});
  stack.push_back({root, 0});
  bool root_virtual = virtual_fs(root);
  while (!stack.empty() && !stop_) {
    Dir d = stack.back();
    stack.pop_back();
    {
      std::lock_guard<std::mutex> l(mtx_);
      current_ = d.path;
    }
    DIR *dir = opendir(d.path.c_str());
    if (!dir) continue;
    std::vector<Dir> subdirs;
    while (dirent *e = readdir(dir)) {
      if (stop_) break;
      const char *n = e->d_name;
      if (!strcmp(n, ".") || !strcmp(n, "..")) continue;
      if (!q.hidden && n[0] == '.') continue;
      scanned_++;
      std::string path = d.path == "/" ? "/" + std::string(n) : d.path + "/" + n;
      struct stat lst, st;
      if (lstat(path.c_str(), &lst) != 0) continue;
      bool link = S_ISLNK(lst.st_mode);
      st = lst;
      if (link && stat(path.c_str(), &st) != 0) st = lst; // broken link
      bool is_dir = S_ISDIR(st.st_mode);
      // descend
      if (is_dir && (q.max_depth < 0 || d.depth < q.max_depth) && (!link || q.follow_links) &&
          (root_virtual || !virtual_fs(path))) {
        if (visited.insert({st.st_dev, st.st_ino}).second) subdirs.push_back({path, d.depth + 1});
      }
      // match
      if (q.kind == Query::FILES && is_dir) continue;
      if (q.kind == Query::FOLDERS && !is_dir) continue;
      if (!match(n)) continue;
      if (!is_dir) {
        if (q.min_size >= 0 && st.st_size < q.min_size) continue;
        if (q.max_size >= 0 && st.st_size > q.max_size) continue;
      } else if (q.min_size >= 0 || q.max_size >= 0 || !q.content.empty()) {
        continue; // size and content filters only apply to files
      }
      if (q.newer_than && st.st_mtime < q.newer_than) continue;
      if (!q.content.empty() && !(S_ISREG(st.st_mode) && file_contains(path, q.content, q.match_case))) continue;
      {
        std::lock_guard<std::mutex> l(mtx_);
        pending_.push_back(path);
      }
      if (++found_ >= q.max_results) {
        truncated_ = true;
        stop_ = true;
        break;
      }
    }
    closedir(dir);
    // visit subfolders in name order (the stack pops from the back)
    std::sort(subdirs.begin(), subdirs.end(), [](const Dir &a, const Dir &b) { return a.path > b.path; });
    for (auto &s : subdirs) stack.push_back(s);
  }
  running_ = false;
}

std::vector<std::string> Search::run(const Query &q) {
  Search s;
  std::vector<std::string> out;
  if (!s.start(q)) return out;
  if (s.worker_.joinable()) s.worker_.join();
  s.take(out);
  return out;
}

} // namespace search
