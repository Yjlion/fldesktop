// fl-launcher: calculator, unit conversion, commands and file results
#include <cstdlib>
#include "fld/util.h"
#include "launcher_providers.h"
#include "test.h"

using namespace launcher;

int main() {
  Extra e;
  CHECK(calculate("2*(3+4)", e));
  CHECK_EQ(e.title, std::string("= 14"));
  CHECK_EQ(e.copy, std::string("14"));
  CHECK(calculate("= 10/4", e));
  CHECK_EQ(e.copy, std::string("2.5"));
  CHECK(calculate("sqrt(16)", e));
  CHECK_EQ(e.copy, std::string("4"));
  CHECK(!calculate("42", e));        // a number alone
  CHECK(!calculate("-3", e));
  CHECK(!calculate("firefox", e));   // no digit
  CHECK(!calculate("fl-term 2", e)); // not an expression
  CHECK(!calculate("1/0", e));

  CHECK(convert("10 km in mi", e));
  CHECK_EQ(e.copy, std::string("6.21371192237"));
  CHECK(convert("100 C to F", e));
  CHECK_EQ(e.copy, std::string("212"));
  CHECK(convert("32 f to c", e));
  CHECK_EQ(e.copy, std::string("0"));
  CHECK(convert("1 GiB in MB", e));
  CHECK_EQ(e.copy, std::string("1073.741824"));
  CHECK(convert("90 min to h", e));
  CHECK_EQ(e.title, std::string("= 1.5 h"));
  CHECK(!convert("10 km in kg", e)); // different dimensions
  CHECK(!convert("10 km", e));
  CHECK(!convert("ten km in mi", e));
  CHECK(!convert("10 km in mi please", e));

  CHECK(command(">htop", {"fl-term", "-e"}, e));
  CHECK_EQ(e.title, std::string("htop"));
  CHECK_EQ(e.argv.size(), (size_t)5);
  CHECK_EQ(e.argv[0], std::string("fl-term"));
  CHECK_EQ(e.argv[2], std::string("sh"));
  CHECK(!command(">", {"fl-term", "-e"}, e));
  CHECK(!command("htop", {"fl-term", "-e"}, e));

  auto q = quick("3+4", {});
  CHECK_EQ(q.size(), (size_t)1);
  CHECK_EQ((int)q[0].kind, (int)Extra::Calc);
  CHECK(quick("files", {}).empty());

  CHECK(locate_command("ab", "/usr/bin/locate").empty());
  CHECK(locate_command("notes", "").empty());
  CHECK(locate_command("-rf", "/usr/bin/locate").empty());
  auto lc = locate_command("notes", "/usr/bin/plocate");
  CHECK_EQ(lc.back(), std::string("notes"));
  CHECK_EQ(lc[lc.size() - 2], std::string("--"));
  setenv("HOME", "/home/u", 1);
  auto f = files_from_locate("/home/u/doc/notes.txt\n/usr/share/notes\n/home/u/.cache/notes\n/home/u/notes.md\n", 8);
  CHECK_EQ(f.size(), (size_t)2);
  CHECK_EQ(f[0].title, std::string("notes.txt"));
  CHECK_EQ(f[0].subtitle, std::string("~/doc"));
  CHECK_EQ(f[0].argv[1], std::string("/home/u/doc/notes.txt"));
  CHECK_EQ(files_from_locate("/home/u/a\n/home/u/b\n/home/u/c\n", 2).size(), (size_t)2);
  return test_result("launcher-providers");
}
