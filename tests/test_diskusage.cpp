// fl-diskusage: scanning and the treemap layout
#include <cstdlib>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include "du_model.h"
#include "test.h"

static void write_bytes(const std::string &p, size_t n) {
  int fd = open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  std::string data(n, 'x');
  if (fd >= 0) {
    if (write(fd, data.data(), n) != (ssize_t)n) {}
    close(fd);
  }
}

int main() {
  char tmpl[] = "/tmp/fld-du-XXXXXX";
  std::string t = mkdtemp(tmpl);
  mkdir((t + "/big").c_str(), 0755);
  mkdir((t + "/small").c_str(), 0755);
  write_bytes(t + "/big/a", 400000);
  write_bytes(t + "/big/b", 200000);
  write_bytes(t + "/small/c", 10000);
  write_bytes(t + "/d", 100000);
  if (symlink("/usr", (t + "/link").c_str()) != 0) {}
  du::Progress pr;
  auto root = du::scan(t, pr);
  CHECK(!root->error);
  CHECK_EQ(root->files, (uint64_t)5); // the symbolic link counts as a file, not followed
  CHECK_EQ(pr.files.load(), (uint64_t)5);
  CHECK_EQ(root->children[0]->name, std::string("big")); // biggest first
  CHECK(root->children[0]->size >= 600000);
  CHECK(root->size >= root->children[0]->size + root->children[1]->size);
  CHECK_EQ(root->children[0]->children[0]->path(), t + "/big/a");
  CHECK(root->children[0]->dir);

  // the layout fills the rectangle and keeps the proportions
  auto tiles = du::layout(*root, {0, 0, 400, 300}, 1, 16, 0);
  double area = 0;
  for (auto &x : tiles) {
    CHECK(x.r.x >= -0.01 && x.r.y >= -0.01 && x.r.x + x.r.w <= 400.01 && x.r.y + x.r.h <= 300.01);
    area += x.r.w * x.r.h;
  }
  CHECK(area > 400 * 300 * 0.999 && area < 400 * 300 * 1.001);
  const du::Tile *big = nullptr;
  for (auto &x : tiles)
    if (x.node->name == "big") big = &x;
  CHECK(big != nullptr);
  if (big) {
    double share = big->r.w * big->r.h / (400.0 * 300);
    double want = (double)root->children[0]->size / root->size;
    CHECK(share > want - 0.01 && share < want + 0.01);
  }
  // two levels: big's files inside it, below its header
  auto deep = du::layout(*root, {0, 0, 400, 300}, 2, 16, 0);
  int inner = 0;
  for (auto &x : deep)
    if (x.depth == 1) inner++;
  CHECK(inner >= 2);
  // squarified: no sliver for comparable sizes
  for (auto &x : tiles) {
    double ratio = std::max(x.r.w / x.r.h, x.r.h / x.r.w);
    if (x.node->size > root->size / 10) CHECK(ratio < 6);
  }
  CHECK_EQ(du::human(999), std::string("999 B"));
  CHECK_EQ(du::human(1500000), std::string("1.5 MB"));
  CHECK_EQ(du::human(25000000000ULL), std::string("25 GB"));
  du::Progress p2;
  CHECK(du::scan(t + "/missing", p2)->error);
  std::string cmd = "rm -rf " + t;
  if (system(cmd.c_str()) != 0) {}
  return test_result("diskusage");
}
