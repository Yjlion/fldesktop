#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#include "search.h"
#include "test.h"

using namespace search;

static std::string g_root;

static void mk(const std::string &rel, const std::string &content = "", bool dir = false) {
  std::string p = g_root + "/" + rel;
  for (size_t i = g_root.size() + 1; (i = p.find('/', i)) != std::string::npos; i++) mkdir(p.substr(0, i).c_str(), 0755);
  if (dir) mkdir(p.c_str(), 0755);
  else std::ofstream(p) << content;
}

static std::vector<std::string> rel(std::vector<std::string> v) {
  for (auto &p : v) p = p.substr(g_root.size() + 1);
  std::sort(v.begin(), v.end());
  return v;
}

static std::string j(const std::vector<std::string> &v) {
  std::string s;
  for (auto &x : v) s += (s.empty() ? "" : " ") + x;
  return s;
}

int main() {
  // ---- name matching
  Query q;
  q.name = "Repo";
  CHECK(NameMatcher(q)("my-repo.txt"));
  q.match_case = true;
  CHECK(!NameMatcher(q)("my-repo.txt"));
  q.match_case = false;
  q.name = "*.TXT";
  CHECK(NameMatcher(q)("a.txt"));
  CHECK(!NameMatcher(q)("a.txt.bak"));
  q.name = "^a.*\\d$";
  q.name_regex = true;
  CHECK(NameMatcher(q)("abc1"));
  CHECK(!NameMatcher(q)("abc"));
  q.name = "(";
  CHECK(!NameMatcher(q).valid());

  // ---- a tree
  char tmpl[] = "/tmp/fld-search-XXXXXX";
  g_root = mkdtemp(tmpl);
  mk("notes.txt", "Shopping list: apples, BREAD\n");
  mk("docs/report.txt", "quarterly figures");
  mk("docs/report.md", "# Report\nnothing here");
  mk("docs/deep/a/b/needle.txt", std::string(70000, 'x') + "the needle is here");
  mk("docs/Photos", "", true);
  mk(".hidden/secret.txt", "bread");
  mk("bin/data.bin", std::string("bread\0\0binary", 13));
  mk("big.log", std::string(5000, 'y'));
  symlink((g_root + "/docs").c_str(), (g_root + "/loop").c_str());
  // an old file
  struct utimbuf ut{1000000000, 1000000000};
  utime((g_root + "/docs/report.md").c_str(), &ut);

  CHECK(file_contains(g_root + "/notes.txt", "bread", false));
  CHECK(!file_contains(g_root + "/notes.txt", "bread", true));
  CHECK(file_contains(g_root + "/docs/deep/a/b/needle.txt", "needle is here", true)); // past the first chunk
  CHECK(!file_contains(g_root + "/bin/data.bin", "bread", false));                    // binary
  CHECK(!file_contains(g_root + "/notes.txt", "bread", false, 4));                    // too big

  Query b;
  b.root = g_root;
  b.name = "report";
  CHECK_EQ(j(rel(Search::run(b))), std::string("docs/report.md docs/report.txt"));
  b.name = "*.txt";
  CHECK_EQ(j(rel(Search::run(b))), std::string("docs/deep/a/b/needle.txt docs/report.txt notes.txt"));
  b.hidden = true;
  CHECK_EQ(rel(Search::run(b)).size(), (size_t)4);
  b.hidden = false;
  b.max_depth = 1;
  CHECK_EQ(j(rel(Search::run(b))), std::string("docs/report.txt notes.txt"));
  b.max_depth = -1;
  b.name = "";
  b.content = "BREAD";
  CHECK_EQ(j(rel(Search::run(b))), std::string("notes.txt"));
  b.content = "";
  b.kind = Query::FOLDERS;
  CHECK_EQ(j(rel(Search::run(b))), std::string("bin docs docs/Photos docs/deep docs/deep/a docs/deep/a/b loop"));
  b.kind = Query::FILES;
  b.min_size = 4000;
  CHECK_EQ(j(rel(Search::run(b))), std::string("big.log docs/deep/a/b/needle.txt"));
  b.min_size = -1;
  b.max_size = 20;
  CHECK_EQ(j(rel(Search::run(b))), std::string("bin/data.bin docs/report.txt"));
  b.max_size = -1;
  b.newer_than = 1500000000;
  b.name = "report";
  CHECK_EQ(j(rel(Search::run(b))), std::string("docs/report.txt"));
  // symlinked folders are not followed unless asked; loops are cut
  b = Query();
  b.root = g_root;
  b.name = "needle.txt";
  CHECK_EQ(Search::run(b).size(), (size_t)1);
  b.follow_links = true;
  CHECK_EQ(Search::run(b).size(), (size_t)1); // loop/ is the same folder as docs/
  b.max_results = 1;
  b.name = "";
  Search s;
  CHECK(s.start(b));
  while (s.running()) usleep(1000);
  std::vector<std::string> out;
  s.take(out);
  CHECK_EQ(out.size(), (size_t)1);
  CHECK(s.truncated());
  b.name = "[";
  b.name_regex = true;
  CHECK(!s.start(b));
  // cancel a running search
  b = Query();
  b.root = "/usr";
  CHECK(s.start(b));
  s.cancel();
  CHECK(!s.running());

  std::string cmd = "rm -rf " + g_root;
  CHECK_EQ(system(cmd.c_str()), 0);
  return test_result("search");
}
