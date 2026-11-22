#include "layout_bar.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>

#include "fld/theme.h"
#include "fld/widgets.h"

LayoutBar::LayoutBar(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) { box(FL_NO_BOX); }

void LayoutBar::set(const std::vector<Seg> &segs) {
  segs_ = segs;
  bool found = false;
  for (auto &s : segs_) found = found || s.item == selected_;
  if (!found) selected_ = -1;
  redraw();
}

void LayoutBar::select_item(int item) {
  selected_ = item;
  redraw();
}

Fl_Color LayoutBar::fs_color(const std::string &fs) {
  if (fs == "ext4" || fs == "ext3" || fs == "ext2") return fl_rgb_color(0x35, 0x84, 0xe4);
  if (fs == "btrfs") return fl_rgb_color(0x26, 0xa2, 0x69);
  if (fs == "xfs") return fl_rgb_color(0x91, 0x41, 0xac);
  if (fs == "vfat") return fl_rgb_color(0x2e, 0xc2, 0x7e);
  if (fs == "exfat") return fl_rgb_color(0x98, 0x8a, 0x2a);
  if (fs == "ntfs") return fl_rgb_color(0x1c, 0x71, 0xd8);
  if (fs == "swap") return fl_rgb_color(0xe6, 0x61, 0x00);
  if (fs == "crypto_LUKS") return fl_rgb_color(0xc0, 0x1c, 0x28);
  if (fs == "LVM2_member" || fs == "linux_raid_member") return fl_rgb_color(0x86, 0x5e, 0x3c);
  if (fs == "extended") return fl_rgb_color(0x77, 0x76, 0x7b);
  return fl_rgb_color(0x9a, 0x99, 0x96);
}

// widths: every segment gets a minimum, the rest is shared by size
static void share(const std::vector<uint64_t> &sizes, int x, int w, int min_w, std::vector<std::pair<int, int>> &out) {
  out.clear();
  size_t n = sizes.size();
  if (!n) return;
  uint64_t sum = 0;
  for (auto s : sizes) sum += s;
  min_w = std::min(min_w, w / (int)n);
  int rest = w - min_w * (int)n;
  int pos = x;
  double acc = 0;
  for (size_t i = 0; i < n; i++) {
    acc += sum ? (double)rest * sizes[i] / sum : (double)rest / n;
    int end = i + 1 == n ? x + w : x + min_w * (int)(i + 1) + (int)(acc + 0.5);
    out.push_back({pos, std::max(1, end - pos)});
    pos = end;
  }
}

void LayoutBar::layout(std::vector<Box> &out) const {
  out.clear();
  std::vector<size_t> top;
  for (size_t i = 0; i < segs_.size(); i++)
    if (!segs_[i].inner) top.push_back(i);
  std::vector<uint64_t> sizes;
  for (auto i : top) sizes.push_back(segs_[i].size);
  std::vector<std::pair<int, int>> wx;
  share(sizes, x(), w(), 34, wx);
  for (size_t k = 0; k < top.size(); k++) {
    out.push_back({wx[k].first, wx[k].second, top[k]});
    if (!segs_[top[k]].container) continue;
    std::vector<size_t> in;
    for (size_t i = 0; i < segs_.size(); i++)
      if (segs_[i].inner) in.push_back(i);
    std::vector<uint64_t> isz;
    for (auto i : in) isz.push_back(segs_[i].size);
    std::vector<std::pair<int, int>> iw;
    share(isz, wx[k].first + 4, wx[k].second - 8, 28, iw);
    for (size_t j = 0; j < in.size(); j++) out.push_back({iw[j].first, iw[j].second, in[j]});
  }
}

void LayoutBar::draw_seg(const Seg &s, int X, int Y, int W, int H, bool sel) {
  const fld::Palette &p = fld::palette();
  bool free = s.fs == "free";
  Fl_Color c = free ? p.bg : fs_color(s.container ? "extended" : s.fs);
  fl_push_clip(X, Y, W, H);
  if (free) {
    fl_color(p.bg);
    fl_rectf(X, Y, W, H);
    fl_color(fl_color_average(p.fg_dim, p.bg, 0.35f));
    for (int i = -H; i < W; i += 8) fl_line(X + i, Y + H, X + i + H, Y);
  } else {
    fl_color(fl_color_average(c, p.bg2, 0.55f));
    fl_rectf(X, Y, W, H);
    if (s.used >= 0) {
      fl_color(c);
      fl_rectf(X, Y, std::max(0, (int)(W * std::min(1.0, s.used))), H);
    } else if (!s.container) {
      fl_color(fl_color_average(c, p.bg2, 0.8f));
      fl_rectf(X, Y, W, H);
    }
  }
  if (!s.container && W > 30) {
    fl_font(FL_HELVETICA_BOLD, 12);
    fl_color(free ? p.fg_dim : fl_contrast(FL_WHITE, fl_color_average(c, p.bg2, 0.7f)));
    std::string a = fld::escape_at(s.label), b = fld::escape_at(s.sub);
    fl_draw(a.c_str(), X + 4, Y + 2, W - 8, H / 2 - 2, FL_ALIGN_BOTTOM_LEFT | FL_ALIGN_CLIP);
    fl_font(FL_HELVETICA, 11);
    fl_draw(b.c_str(), X + 4, Y + H / 2, W - 8, H / 2 - 2, FL_ALIGN_TOP_LEFT | FL_ALIGN_CLIP);
  }
  fl_pop_clip();
  fl_color(p.border);
  fl_rect(X, Y, W, H);
  if (sel) {
    fl_color(p.accent);
    fl_line_style(FL_SOLID, 3);
    fl_rect(X + 1, Y + 1, W - 2, H - 2);
    fl_line_style(0);
  }
}

void LayoutBar::draw() {
  const fld::Palette &p = fld::palette();
  fl_color(p.bg2);
  fl_rectf(x(), y(), w(), h());
  layout(boxes_);
  if (boxes_.empty()) {
    fl_color(p.fg_dim);
    fl_font(FL_HELVETICA, 13);
    fl_draw("No partitions", x(), y(), w(), h(), FL_ALIGN_CENTER);
    return;
  }
  for (auto &b : boxes_) {
    const Seg &s = segs_[b.seg];
    if (s.inner) draw_seg(s, b.x, y() + 12, b.w, h() - 18, s.item == selected_);
    else draw_seg(s, b.x, y(), b.w, h(), s.item == selected_ && !s.container);
    if (s.container) {
      fl_font(FL_HELVETICA, 10);
      fl_color(FL_WHITE);
      fl_draw("extended", b.x + 4, y(), b.w - 8, 12, FL_ALIGN_LEFT | FL_ALIGN_CLIP);
      if (s.item == selected_) {
        fl_color(p.accent);
        fl_line_style(FL_SOLID, 3);
        fl_rect(b.x + 1, y() + 1, b.w - 2, h() - 2);
        fl_line_style(0);
      }
    }
  }
}

int LayoutBar::handle(int e) {
  if (e == FL_PUSH && Fl::event_button() == FL_LEFT_MOUSE) {
    int ex = Fl::event_x(), ey = Fl::event_y();
    int hit = -1;
    for (auto &b : boxes_) {
      const Seg &s = segs_[b.seg];
      bool inside = ex >= b.x && ex < b.x + b.w && (!s.inner || (ey >= y() + 12 && ey < y() + h() - 6));
      if (inside && (hit < 0 || s.inner)) hit = s.item;
    }
    if (hit >= 0) {
      selected_ = hit;
      redraw();
      do_callback();
    }
    return 1;
  }
  return Fl_Widget::handle(e);
}
