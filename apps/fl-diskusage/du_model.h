// fl-diskusage: scanning a folder tree and laying it out as a treemap (no
// FLTK; unit tested).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace du {

struct Node {
  std::string name;  // the file name ("/" for the root of the scan: its path)
  uint64_t size = 0; // bytes on disk (blocks), the whole subtree for folders
  uint64_t files = 0;
  bool dir = false;
  bool error = false; // could not be read (permission)
  Node *parent = nullptr;
  std::vector<std::unique_ptr<Node>> children; // biggest first after scan()
  std::string path() const;
};

struct Progress {
  std::atomic<uint64_t> files{0}, bytes{0};
  std::atomic<bool> cancel{false};
};

// Scans path (not following symbolic links, staying on its file system).
// Blocking; call it from a thread. Children are sorted biggest first.
std::unique_ptr<Node> scan(const std::string &path, Progress &progress);

struct Rect {
  double x, y, w, h;
};
struct Tile {
  const Node *node;
  Rect r;
  int depth;
};

// Squarified treemap of node's children inside r, down to `levels` levels
// (folders get a header strip of `header` pixels for their name); tiles
// smaller than min_area are left out.
std::vector<Tile> layout(const Node &node, Rect r, int levels, double header = 16, double min_area = 16);

// "1.5 GB"
std::string human(uint64_t bytes);

} // namespace du
