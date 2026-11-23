#include "term_panes.h"

#include <algorithm>
#include <climits>
#include <cwctype>

namespace term {

struct PaneTree::Node {
  Id id = 0; // a pane when a and b are empty
  bool side_by_side = true;
  double ratio = 0.5;
  std::unique_ptr<Node> a, b;
  Node *parent = nullptr;
  bool leaf() const { return !a; }
};

using Node = PaneTree::Node;

PaneTree::PaneTree(Id first) { reset(first); }
PaneTree::~PaneTree() = default;
PaneTree::PaneTree(PaneTree &&) noexcept = default;
PaneTree &PaneTree::operator=(PaneTree &&) noexcept = default;

void PaneTree::reset(Id first) {
  root_ = std::make_unique<Node>();
  root_->id = first;
  drag_ = nullptr;
}

static Node *find(Node *n, PaneTree::Id id) {
  if (!n) return nullptr;
  if (n->leaf()) return n->id == id ? n : nullptr;
  if (Node *r = find(n->a.get(), id)) return r;
  return find(n->b.get(), id);
}

bool PaneTree::split(Id id, Id added, bool side_by_side) {
  Node *n = find(root_.get(), id);
  if (!n || find(root_.get(), added)) return false;
  auto a = std::make_unique<Node>(), b = std::make_unique<Node>();
  a->id = id;
  b->id = added;
  a->parent = b->parent = n;
  n->id = 0;
  n->side_by_side = side_by_side;
  n->ratio = 0.5;
  n->a = std::move(a);
  n->b = std::move(b);
  return true;
}

bool PaneTree::remove(Id id) {
  Node *n = find(root_.get(), id);
  if (!n || !n->parent) return false;
  Node *p = n->parent;
  std::unique_ptr<Node> keep = std::move(p->a.get() == n ? p->b : p->a);
  // the sibling moves up into the parent's place
  p->id = keep->id;
  p->side_by_side = keep->side_by_side;
  p->ratio = keep->ratio;
  p->a = std::move(keep->a);
  p->b = std::move(keep->b);
  if (p->a) p->a->parent = p;
  if (p->b) p->b->parent = p;
  drag_ = nullptr;
  return true;
}

bool PaneTree::contains(Id id) const { return find(root_.get(), id) != nullptr; }

static void collect(const Node *n, std::vector<PaneTree::Id> &out) {
  if (!n) return;
  if (n->leaf()) out.push_back(n->id);
  else {
    collect(n->a.get(), out);
    collect(n->b.get(), out);
  }
}

std::vector<PaneTree::Id> PaneTree::ids() const {
  std::vector<Id> out;
  collect(root_.get(), out);
  return out;
}

size_t PaneTree::count() const { return ids().size(); }

// the two parts of a split's rectangle
static void parts(const Node *n, const Rect &r, int gap, Rect &ra, Rect &rb) {
  ra = rb = r;
  if (n->side_by_side) {
    int aw = std::max(0, (int)((r.w - gap) * n->ratio));
    ra.w = aw;
    rb.x = r.x + aw + gap;
    rb.w = std::max(0, r.w - aw - gap);
  } else {
    int ah = std::max(0, (int)((r.h - gap) * n->ratio));
    ra.h = ah;
    rb.y = r.y + ah + gap;
    rb.h = std::max(0, r.h - ah - gap);
  }
}

static void lay(const Node *n, const Rect &r, int gap, std::vector<std::pair<PaneTree::Id, Rect>> &out) {
  if (n->leaf()) {
    out.push_back({n->id, r});
    return;
  }
  Rect ra, rb;
  parts(n, r, gap, ra, rb);
  lay(n->a.get(), ra, gap, out);
  lay(n->b.get(), rb, gap, out);
}

std::vector<std::pair<PaneTree::Id, Rect>> PaneTree::layout(const Rect &r, int gap) const {
  std::vector<std::pair<Id, Rect>> out;
  lay(root_.get(), r, gap, out);
  return out;
}

static void divs(const Node *n, const Rect &r, int gap, std::vector<Rect> &out) {
  if (n->leaf()) return;
  Rect ra, rb;
  parts(n, r, gap, ra, rb);
  if (n->side_by_side) out.push_back({ra.x + ra.w, r.y, gap, r.h});
  else out.push_back({r.x, ra.y + ra.h, r.w, gap});
  divs(n->a.get(), ra, gap, out);
  divs(n->b.get(), rb, gap, out);
}

std::vector<Rect> PaneTree::dividers(const Rect &r, int gap) const {
  std::vector<Rect> out;
  divs(root_.get(), r, gap, out);
  return out;
}

PaneTree::Id PaneTree::neighbor(Id id, Dir d, const Rect &r, int gap) const {
  auto all = layout(r, gap);
  Rect me;
  bool found = false;
  for (auto &p : all)
    if (p.first == id) {
      me = p.second;
      found = true;
    }
  if (!found) return 0;
  Id best = 0;
  long best_d = LONG_MAX;
  int mcx = me.x + me.w / 2, mcy = me.y + me.h / 2;
  for (auto &p : all) {
    if (p.first == id) continue;
    const Rect &o = p.second;
    bool ok = false;
    switch (d) {
      case Dir::Left: ok = o.x + o.w <= me.x && o.y < me.y + me.h && o.y + o.h > me.y; break;
      case Dir::Right: ok = o.x >= me.x + me.w && o.y < me.y + me.h && o.y + o.h > me.y; break;
      case Dir::Up: ok = o.y + o.h <= me.y && o.x < me.x + me.w && o.x + o.w > me.x; break;
      case Dir::Down: ok = o.y >= me.y + me.h && o.x < me.x + me.w && o.x + o.w > me.x; break;
    }
    if (!ok) continue;
    long dx = o.x + o.w / 2 - mcx, dy = o.y + o.h / 2 - mcy;
    long dist = dx * dx + dy * dy;
    if (dist < best_d) {
      best_d = dist;
      best = p.first;
    }
  }
  return best;
}

// the split whose divider is under (x, y), and its rectangle
static Node *hit(Node *n, const Rect &r, int gap, int x, int y, Rect &nr) {
  if (!n || n->leaf()) return nullptr;
  Rect ra, rb;
  parts(n, r, gap, ra, rb);
  bool on = n->side_by_side ? (x >= ra.x + ra.w && x < rb.x && y >= r.y && y < r.y + r.h)
                            : (y >= ra.y + ra.h && y < rb.y && x >= r.x && x < r.x + r.w);
  if (on) {
    nr = r;
    return n;
  }
  if (Node *h = hit(n->a.get(), ra, gap, x, y, nr)) return h;
  return hit(n->b.get(), rb, gap, x, y, nr);
}

bool PaneTree::divider_at(const Rect &r, int gap, int x, int y, bool *vertical_bar) const {
  Rect nr;
  Node *n = hit(root_.get(), r, gap, x, y, nr);
  if (n && vertical_bar) *vertical_bar = n->side_by_side;
  return n != nullptr;
}

bool PaneTree::drag_begin(const Rect &r, int gap, int x, int y) {
  drag_ = hit(root_.get(), r, gap, x, y, drag_rect_);
  return drag_ != nullptr;
}

void PaneTree::drag_to(int x, int y) {
  if (!drag_) return;
  const Rect &r = drag_rect_;
  double v = drag_->side_by_side ? (r.w > 0 ? (double)(x - r.x) / r.w : 0.5) : (r.h > 0 ? (double)(y - r.y) / r.h : 0.5);
  drag_->ratio = std::max(0.1, std::min(0.9, v));
}

void PaneTree::drag_end() { drag_ = nullptr; }

static void desc(const Node *n, std::string &out) {
  if (n->leaf()) {
    out += std::to_string((long long)n->id);
    return;
  }
  out += n->side_by_side ? "(h " : "(v ";
  desc(n->a.get(), out);
  out += ' ';
  desc(n->b.get(), out);
  out += ')';
}

std::string PaneTree::describe() const {
  std::string s;
  desc(root_.get(), s);
  return s;
}

// ---- search ----------------------------------------------------------------------

std::u32string to_u32(const std::string &s) {
  std::u32string out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    char32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n && i + (size_t)k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + (size_t)k] & 0x3F);
    out += cp;
    i += (size_t)n;
  }
  return out;
}

static char32_t fold(char32_t c) { return c < 0x250 ? (char32_t)std::towlower((wint_t)c) : c; }

static bool match_at(const std::u32string &line, size_t pos, const std::u32string &needle, bool icase) {
  if (pos + needle.size() > line.size()) return false;
  for (size_t k = 0; k < needle.size(); k++) {
    char32_t a = line[pos + k], b = needle[k];
    if (icase ? fold(a) != fold(b) : a != b) return false;
  }
  return true;
}

Match find_in_lines(const std::vector<std::u32string> &lines, const std::u32string &needle, long from_line,
                    int from_col, bool backwards, bool icase) {
  Match m;
  long n = (long)lines.size();
  if (needle.empty() || n == 0) return m;
  from_line = std::max(0L, std::min(from_line, n - 1));
  for (long step = 0; step <= n; step++) {
    long li = backwards ? ((from_line - step) % n + n) % n : (from_line + step) % n;
    const std::u32string &l = lines[(size_t)li];
    if (l.size() < needle.size()) continue;
    long last = (long)(l.size() - needle.size());
    if (!backwards) {
      long start = step == 0 ? from_col + 1 : 0;
      if (step == n) last = std::min(last, (long)from_col); // wrapped back to the start line
      for (long c = std::max(0L, start); c <= last; c++)
        if (match_at(l, (size_t)c, needle, icase)) return {li, (int)c, (int)needle.size()};
    } else {
      long start = step == 0 ? from_col - 1 : last;
      long stop = step == n ? from_col : 0;
      for (long c = std::min(start, last); c >= stop; c--)
        if (match_at(l, (size_t)c, needle, icase)) return {li, (int)c, (int)needle.size()};
    }
  }
  return m;
}

} // namespace term
