#include "fld/git_status.h"

#include <algorithm>
#include <cstdlib>

#include "fld/util.h"

namespace fld {
namespace git {

namespace {

State from_xy(char x, char y) {
  if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) return State::Conflicted;
  if (x == 'R' || x == 'C') return State::Renamed;
  if (x == 'A') return State::Added;
  if (x == 'D' || y == 'D') return State::Deleted;
  if (x == 'M' || y == 'M' || x == 'T' || y == 'T') return State::Modified;
  return State::Clean;
}

// the n-th space separated field and what follows the last one
bool fields(const std::string &rec, int n, std::vector<std::string> &out, std::string &rest) {
  out.clear();
  size_t p = 0;
  for (int i = 0; i < n; i++) {
    size_t sp = rec.find(' ', p);
    if (sp == std::string::npos) return false;
    out.push_back(rec.substr(p, sp - p));
    p = sp + 1;
  }
  rest = rec.substr(p);
  return true;
}

}  // namespace

std::map<std::string, State> parse_status(const std::string &z) {
  std::map<std::string, State> out;
  std::vector<std::string> recs;
  size_t p = 0;
  while (p < z.size()) {
    size_t e = z.find('\0', p);
    if (e == std::string::npos) e = z.size();
    recs.push_back(z.substr(p, e - p));
    p = e + 1;
  }
  std::vector<std::string> f;
  std::string path;
  for (size_t i = 0; i < recs.size(); i++) {
    const std::string &r = recs[i];
    if (r.size() < 3 || r[1] != ' ') continue;
    switch (r[0]) {
      case '1':  // 1 XY sub mH mI mW hH hI path
        if (fields(r, 8, f, path) && f[1].size() == 2 && !path.empty()) out[path] = from_xy(f[1][0], f[1][1]);
        break;
      case '2':  // 2 XY sub mH mI mW hH hI Xscore path, then the original path
        if (fields(r, 9, f, path) && f[1].size() == 2 && !path.empty()) out[path] = from_xy(f[1][0], f[1][1]);
        i++;  // the original path's record
        break;
      case 'u':  // u XY sub m1 m2 m3 mW h1 h2 h3 path
        if (fields(r, 10, f, path) && !path.empty()) out[path] = State::Conflicted;
        break;
      case '?':
        out[r.substr(2)] = State::Untracked;
        break;
      case '!':
        out[r.substr(2)] = State::Ignored;
        break;
      default:
        break;
    }
  }
  return out;
}

State state_of(const std::map<std::string, State> &status, const std::string &rel, bool is_dir) {
  if (!is_dir) {
    auto it = status.find(rel);
    if (it != status.end()) return it->second;
    // an untracked or ignored folder is listed with a slash: its files share it
    for (size_t s = rel.rfind('/'); s != std::string::npos && s > 0; s = rel.rfind('/', s - 1)) {
      auto d = status.find(rel.substr(0, s + 1));
      if (d != status.end() && (d->second == State::Untracked || d->second == State::Ignored)) return d->second;
      if (s == 0) break;
    }
    return State::Clean;
  }
  std::string prefix = rel.empty() ? "" : rel + "/";
  auto whole = status.find(prefix);
  if (whole != status.end()) return whole->second;  // an untracked or ignored folder
  State st = State::Clean;
  for (auto it = status.lower_bound(prefix); it != status.end() && it->first.compare(0, prefix.size(), prefix) == 0;
       ++it) {
    if (it->second == State::Ignored) continue;
    if (it->second == State::Conflicted) return State::Conflicted;
    st = State::Modified;
  }
  return st;
}

std::vector<Hunk> parse_diff(const std::string &diff) {
  std::vector<Hunk> out;
  size_t p = 0;
  while (p < diff.size()) {
    size_t e = diff.find('\n', p);
    if (e == std::string::npos) e = diff.size();
    std::string line = diff.substr(p, e - p);
    p = e + 1;
    // @@ -a[,b] +c[,d] @@
    if (line.compare(0, 4, "@@ -") != 0) continue;
    const char *s = line.c_str() + 4;
    char *end;
    long a = strtol(s, &end, 10), b = 1;
    (void)a;
    if (*end == ',') b = strtol(end + 1, &end, 10);
    if (*end != ' ' || end[1] != '+') continue;
    long c = strtol(end + 2, &end, 10), d = 1;
    if (*end == ',') d = strtol(end + 1, &end, 10);
    if (*end != ' ' || b < 0 || d < 0 || c < 0 || c > 100000000 || d > 100000000) continue;
    if (d == 0) out.push_back({Hunk::Deleted, (int)std::max(1L, c), 0});
    else if (b == 0) out.push_back({Hunk::Added, (int)c, (int)d});
    else out.push_back({Hunk::Changed, (int)c, (int)d});
  }
  return out;
}

std::string work_tree(const std::string &dir) {
  if (which("git").empty()) return "";
  std::string out;
  if (run_capture({"git", "-C", dir, "rev-parse", "--show-toplevel"}, out) != 0) return "";
  return trim(out);
}

std::map<std::string, State> status(const std::string &top) {
  std::string out;
  if (top.empty() ||
      run_capture({"git", "-C", top, "status", "--porcelain=v2", "-z", "--untracked-files=normal"}, out) != 0)
    return {};
  return parse_status(out);
}

std::vector<Hunk> diff_lines(const std::string &file) {
  std::string out;
  std::string dir = dirname(file);
  if (work_tree(dir).empty()) return {};
  if (run_capture({"git", "-C", dir, "diff", "--no-color", "--no-ext-diff", "-U0", "--", basename(file)}, out) != 0)
    return {};
  return parse_diff(out);
}

}  // namespace git
}  // namespace fld
