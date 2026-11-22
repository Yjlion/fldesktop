// The partitions of a disk as a bar: each segment as wide as its share of
// the disk (with a minimum width), colored by file system, the used part
// shaded, free space hatched. Logical partitions sit inside their extended
// partition. Click to select a segment.
#pragma once
#include <FL/Fl_Widget.H>
#include <cstdint>
#include <string>
#include <vector>

class LayoutBar : public Fl_Widget {
public:
  struct Seg {
    uint64_t start = 0, size = 0;
    std::string label, sub;  // name, size
    std::string fs;          // for the color; "" = none, "free" = free space
    double used = -1;        // 0..1, -1 unknown
    bool inner = false;      // inside the extended partition
    bool container = false;  // the extended partition
    int item = -1;           // the caller's index
  };
  LayoutBar(int x, int y, int w, int h);
  void set(const std::vector<Seg> &segs);
  void select_item(int item);
  int selected_item() const { return selected_; }
  static Fl_Color fs_color(const std::string &fs);
  int handle(int e) override;

protected:
  void draw() override;

private:
  struct Box {
    int x, w;
    size_t seg;
  };
  std::vector<Seg> segs_;
  std::vector<Box> boxes_;  // laid out in draw()
  int selected_ = -1;
  void layout(std::vector<Box> &out) const;
  void draw_seg(const Seg &s, int x, int y, int w, int h, bool sel);
};
