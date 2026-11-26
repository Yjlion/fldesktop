// libfld git_status: the porcelain v2 and diff parsers, and a real repository
#include <cstdlib>
#include <string>

#include "fld/git_status.h"
#include "fld/util.h"
#include "test.h"

using namespace fld::git;

static std::string z(std::initializer_list<const char *> recs) {
  std::string s;
  for (auto r : recs) {
    s += r;
    s += '\0';
  }
  return s;
}

int main() {
  auto st = parse_status(z({
      "1 .M N... 100644 100644 100644 abc abc src/main.cpp",
      "1 A. N... 000000 100644 100644 000 abd new file.txt",
      "1 .D N... 100644 100644 000000 abc abc gone.c",
      "2 R. N... 100644 100644 100644 abc abc R100 doc/new.md", "doc/old.md",
      "u UU N... 100644 100644 100644 100644 a b c conflict.h",
      "? notes/",
      "? todo.txt",
      "! build/",
  }));
  CHECK(st["src/main.cpp"] == State::Modified);
  CHECK(st["new file.txt"] == State::Added);  // spaces in names
  CHECK(st["gone.c"] == State::Deleted);
  CHECK(st["doc/new.md"] == State::Renamed);
  CHECK(st.count("doc/old.md") == 0);  // the original path is not an entry
  CHECK(st["conflict.h"] == State::Conflicted);
  CHECK(st["notes/"] == State::Untracked);
  CHECK(st["todo.txt"] == State::Untracked);
  CHECK(st["build/"] == State::Ignored);

  CHECK(state_of(st, "src/main.cpp", false) == State::Modified);
  CHECK(state_of(st, "src/other.cpp", false) == State::Clean);
  CHECK(state_of(st, "src", true) == State::Modified);
  CHECK(state_of(st, "doc", true) == State::Modified);
  CHECK(state_of(st, "notes", true) == State::Untracked);
  CHECK(state_of(st, "notes/a/b.txt", false) == State::Untracked);  // inside an untracked folder
  CHECK(state_of(st, "build", true) == State::Ignored);
  CHECK(state_of(st, "build/x.o", false) == State::Ignored);
  CHECK(state_of(st, "", true) == State::Conflicted);  // the top: the conflict wins
  CHECK(state_of(st, "lib", true) == State::Clean);
  CHECK(parse_status("").empty());
  CHECK(parse_status(std::string("1 XY", 4)).empty());  // truncated

  auto h = parse_diff("diff --git a/f b/f\n--- a/f\n+++ b/f\n"
                      "@@ -3 +3 @@ ctx\n-a\n+b\n"
                      "@@ -10,0 +11,2 @@\n+x\n+y\n"
                      "@@ -20,3 +21,0 @@\n-p\n-q\n-r\n"
                      "@@ -0,0 +1 @@\n+first\n"
                      "@@ -1,2 +0,0 @@\n");
  CHECK_EQ(h.size(), (size_t)5);
  if (h.size() == 5) {
    CHECK(h[0].kind == Hunk::Changed && h[0].line == 3 && h[0].count == 1);
    CHECK(h[1].kind == Hunk::Added && h[1].line == 11 && h[1].count == 2);
    CHECK(h[2].kind == Hunk::Deleted && h[2].line == 21 && h[2].count == 0);
    CHECK(h[3].kind == Hunk::Added && h[3].line == 1 && h[3].count == 1);
    CHECK(h[4].kind == Hunk::Deleted && h[4].line == 1);  // everything gone: marked at line 1
  }
  CHECK(parse_diff("@@ -a +b @@\n@@ -1,-2 +1 @@\n").empty());

  // a real repository, when git is installed
  if (!fld::which("git").empty()) {
    char tmpl[] = "/tmp/fld-git-XXXXXX";
    std::string dir = mkdtemp(tmpl), out;
    setenv("GIT_CONFIG_NOSYSTEM", "1", 1);
    setenv("HOME", dir.c_str(), 1);
    fld::run_capture({"git", "-C", dir, "init", "-q"}, out);
    fld::write_file(dir + "/a.txt", "one\ntwo\nthree\n");
    fld::run_capture({"git", "-C", dir, "add", "a.txt"}, out);
    fld::run_capture({"git", "-C", dir, "-c", "user.name=t", "-c", "user.email=t@t", "commit", "-qm", "x"}, out);
    fld::write_file(dir + "/a.txt", "one\nTWO\nthree\nfour\n");
    fld::write_file(dir + "/b.txt", "new\n");
    std::string top = work_tree(dir);
    CHECK(!top.empty());
    auto s2 = status(top);
    CHECK(s2["a.txt"] == State::Modified);
    CHECK(s2["b.txt"] == State::Untracked);
    auto d = diff_lines(dir + "/a.txt");
    CHECK_EQ(d.size(), (size_t)2);
    if (d.size() == 2) {
      CHECK(d[0].kind == Hunk::Changed && d[0].line == 2);
      CHECK(d[1].kind == Hunk::Added && d[1].line == 4);
    }
    CHECK(work_tree("/").empty());
    fld::run_capture({"rm", "-rf", dir}, out);
  }
  return test_result("git-status");
}
