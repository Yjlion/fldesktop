#include "du_model.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace du {

std::string Node::path() const {
  if (!parent) return name;
  std::string p = parent->path();
  return (p.size() > 1 && p.back() == '/') || p == "/" ? p + name : p + "/" + name;
}

static void scan_dir(int dirfd, Node &n, dev_t dev, Progress &pr, int depth) {
  DIR *d = fdopendir(dirfd);
  if (!d) {
    close(dirfd);
    n.error = true;
    return;
  }
  while (dirent *e = readdir(d)) {
    if (pr.cancel) break;
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    struct stat st;
    if (fstatat(dirfd, e->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    auto c = std::make_unique<Node>();
    c->name = e->d_name;
    c->parent = &n;
    c->size = (uint64_t)st.st_blocks * 512;
    if (S_ISDIR(st.st_mode)) {
      c->dir = true;
      // other file systems (mounts) and very deep trees are left out
      if (st.st_dev == dev && depth < 64) {
        int fd = openat(dirfd, e->d_name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (fd >= 0) scan_dir(fd, *c, dev, pr, depth + 1);
        else c->error = true;
      }
    } else {
      c->files = 1;
      pr.files++;
      pr.bytes += c->size;
    }
    n.size += c->size;
    n.files += c->files;
    n.children.push_back(std::move(c));
  }
  closedir(d);
  std::sort(n.children.begin(), n.children.end(),
            [](const std::unique_ptr<Node> &a, const std::unique_ptr<Node> &b) { return a->size > b->size; });
}

std::unique_ptr<Node> scan(const std::string &path, Progress &progress) {
  auto root = std::make_unique<Node>();
  root->name = path;
  root->dir = true;
  struct stat st;
  int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0 || fstat(fd, &st) != 0) {
    if (fd >= 0) close(fd);
    root->error = true;
    return root;
  }
  root->size = (uint64_t)st.st_blocks * 512;
  scan_dir(fd, *root, st.st_dev, progress, 0);
  return root;
}

// ---- squarified treemap (Bruls, Huizing, van Wijk) --------------------------

namespace {

double worst(const std::vector<double> &row, double side) {
  double s = 0, mx = 0, mn = 1e300;
  for (double a : row) {
    s += a;
    mx = std::max(mx, a);
    mn = std::min(mn, a);
  }
  if (s <= 0 || mn <= 0) return 1e300;
  double s2 = s * s, w2 = side * side;
  return std::max(w2 * mx / s2, s2 / (w2 * mn));
}

void place(const Node &parent, Rect r, int depth, int levels, double header, double min_area,
           std::vector<Tile> &out) {
  if (r.w < 2 || r.h < 2 || parent.size == 0) return;
  // the children in proportion to the area (biggest first, as scan() sorts)
  std::vector<const Node *> items;
  for (auto &c : parent.children)
    if (c->size > 0) items.push_back(c.get());
  if (items.empty()) return;
  double total = 0;
  for (auto *c : items) total += (double)c->size;
  double scale = r.w * r.h / total;
  size_t i = 0;
  while (i < items.size()) {
    double side = std::min(r.w, r.h);
    std::vector<double> row;
    size_t j = i;
    while (j < items.size()) {
      std::vector<double> next = row;
      next.push_back(items[j]->size * scale);
      if (!row.empty() && worst(next, side) > worst(row, side)) break;
      row = next;
      j++;
    }
    double sum = 0;
    for (double a : row) sum += a;
    bool horizontal = r.w >= r.h; // the row is a column on the left, else a row on top
    double thick = sum / side;
    double off = 0;
    for (size_t k = i; k < j; k++) {
      double len = row[k - i] / thick;
      Rect t = horizontal ? Rect{r.x, r.y + off, thick, len} : Rect{r.x + off, r.y, len, thick};
      off += len;
      if (t.w * t.h < min_area) continue;
      out.push_back({items[k], t, depth});
      if (items[k]->dir && depth + 1 < levels && t.h > header + 4 && t.w > 8)
        place(*items[k], Rect{t.x + 1, t.y + header, t.w - 2, t.h - header - 1}, depth + 1, levels, header,
              min_area, out);
    }
    if (horizontal) {
      r.x += thick;
      r.w -= thick;
    } else {
      r.y += thick;
      r.h -= thick;
    }
    i = j;
    if (r.w < 1 || r.h < 1) break;
  }
}

} // namespace

std::vector<Tile> layout(const Node &node, Rect r, int levels, double header, double min_area) {
  std::vector<Tile> out;
  place(node, r, 0, levels, header, min_area, out);
  return out;
}

std::string human(uint64_t b) {
  const char *u[] = {"B", "KB", "MB", "GB", "TB", "PB"};
  double v = (double)b;
  int i = 0;
  while (v >= 1000 && i < 5) {
    v /= 1000;
    i++;
  }
  char buf[32];
  if (i == 0) snprintf(buf, sizeof buf, "%llu B", (unsigned long long)b);
  else snprintf(buf, sizeof buf, v < 10 ? "%.1f %s" : "%.0f %s", v, u[i]);
  return buf;
}

} // namespace du
