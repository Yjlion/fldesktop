// fl-edit: the language server client against tests/tools/fake-lsp.py
#include "lsp.h"
#include "test.h"

#include <FL/Fl.H>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

static bool wait_for(const std::function<bool()> &done) {
  time_t end = time(nullptr) + 10;
  while (!done() && time(nullptr) < end) Fl::wait(0.1);
  return done();
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  std::string server = argv[1];
  if (access("/usr/bin/python3", X_OK) != 0 && system("command -v python3 >/dev/null") != 0) return 77;
  char tmpl[] = "/tmp/fl-lsp-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  std::string file = dir + "/a b.c";
  CHECK_EQ(lspc::project_root(file), dir);

  lspc::set_command_resolver([server](const std::string &id) {
    return id == "cpp" ? std::vector<std::string>{"python3", server} : std::vector<std::string>{};
  });
  std::vector<lsp::Diagnostic> got;
  std::string got_path;
  lspc::set_diagnostics_handler([&](const std::string &p, const std::vector<lsp::Diagnostic> &d) {
    got_path = p;
    got = d;
  });
  CHECK(lspc::attach(file, "python", "x") == nullptr); // no server for it
  lspc::Client *c = lspc::attach(file, "cpp", "int main;\nint x;\n");
  CHECK(c != nullptr);
  CHECK_EQ(lspc::name(c), std::string("python3"));
  CHECK(wait_for([&] { return !got.empty(); }));
  CHECK_EQ(got_path, file);
  CHECK_EQ(got[0].message, std::string("fake: 9 characters"));
  CHECK_EQ(got[0].severity, 2);
  // the same server for a second file in the folder
  CHECK(lspc::attach(dir + "/b.c", "cpp", "") == c);

  got.clear();
  lspc::change(c, file, "int main2;\n");
  // the answer to the change (late answers to the opening may come first)
  CHECK(wait_for([&] { return !got.empty() && got[0].message == "fake: 10 characters"; }));
  CHECK_EQ(got[0].end_col, 10);

  std::vector<lsp::CompletionItem> items;
  bool answered = false;
  lspc::complete(c, file, 0, 3, [&](const std::vector<lsp::CompletionItem> &it) {
    items = it;
    answered = true;
  });
  CHECK(wait_for([&] { return answered; }));
  CHECK_EQ(items.size(), (size_t)2);
  CHECK_EQ(items[1].insert, std::string("fake_two()"));

  std::vector<lsp::Location> locs;
  answered = false;
  lspc::definition(c, file, 1, 1, [&](const std::vector<lsp::Location> &l) {
    locs = l;
    answered = true;
  });
  CHECK(wait_for([&] { return answered; }));
  CHECK_EQ(locs.size(), (size_t)1);
  CHECK_EQ(locs[0].path, file);
  CHECK_EQ(locs[0].line, 2);

  lspc::closed(c, file);
  lspc::shutdown_all();
  CHECK_EQ(lspc::name(c), std::string(""));
  rmdir(dir.c_str());
  return test_result("lsp-client");
}
