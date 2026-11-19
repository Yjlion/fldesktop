// fl-web: address bar input, bookmarks and history files
#include <unistd.h>

#include "browser_util.h"
#include "fld/util.h"
#include "test.h"

int main() {
  using namespace web;
  const std::string ddg = search_template("DuckDuckGo");
  CHECK_EQ(ddg, std::string("https://duckduckgo.com/?q=%s"));
  CHECK_EQ(search_template("google"), std::string("https://www.google.com/search?q=%s"));
  CHECK_EQ(search_template("https://s.example/?q=%s"), std::string("https://s.example/?q=%s"));
  CHECK_EQ(search_template("nonsense"), ddg);
  CHECK_EQ(url_from_input("example.com", ddg), std::string("https://example.com"));
  CHECK_EQ(url_from_input("  example.com/a?b=c ", ddg), std::string("https://example.com/a?b=c"));
  CHECK_EQ(url_from_input("http://example.com", ddg), std::string("http://example.com"));
  CHECK_EQ(url_from_input("about:blank", ddg), std::string("about:blank"));
  CHECK_EQ(url_from_input("localhost:8080", ddg), std::string("http://localhost:8080"));
  CHECK_EQ(url_from_input("192.168.1.1/admin", ddg), std::string("http://192.168.1.1/admin"));
  CHECK_EQ(url_from_input("/tmp/a b.html", ddg), std::string("file:///tmp/a%20b.html"));
  CHECK_EQ(url_from_input("hello world", ddg), std::string("https://duckduckgo.com/?q=hello+world"));
  CHECK_EQ(url_from_input("3.14", ddg), std::string("https://duckduckgo.com/?q=3.14"));
  CHECK_EQ(url_from_input("c++ & rust", ddg), std::string("https://duckduckgo.com/?q=c%2B%2B+%26+rust"));
  CHECK_EQ(url_from_input("fltk", ddg), std::string("https://duckduckgo.com/?q=fltk"));
  CHECK_EQ(url_from_input("", ddg), std::string(""));
  CHECK_EQ(url_host("https://user@www.example.com:8443/x"), std::string("www.example.com"));
  CHECK_EQ(url_host("about:blank"), std::string(""));
  CHECK_EQ(fallback_title("https://www.example.com/x"), std::string("example.com"));
  CHECK_EQ(fallback_title("file:///home/me/a%20b.html"), std::string("a b.html"));

  char tmpl[] = "/tmp/fl-web-test-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  std::string bm = dir + "/bookmarks.tsv", hist = dir + "/history.tsv";
  CHECK(save_bookmarks(bm, {{"https://a.example/", "A\tsite"}, {"https://b.example/", ""}}));
  auto b = load_bookmarks(bm);
  CHECK_EQ(b.size(), (size_t)2);
  if (b.size() == 2) {
    CHECK_EQ(b[0].title, std::string("A site"));
    CHECK_EQ(b[1].url, std::string("https://b.example/"));
  }
  for (int i = 0; i < 30; i++)
    add_history(hist, {(time_t)(1000 + i), "https://h.example/" + std::to_string(i % 10), "Page " + std::to_string(i % 10)}, 8);
  auto h = load_history(hist);
  CHECK(!h.empty());
  auto view = history_view(h, "", 100);
  CHECK(!view.empty() && view.size() <= 10);
  if (!view.empty()) CHECK_EQ(view[0].url, std::string("https://h.example/9"));
  CHECK_EQ(history_view(h, "page 3", 100).size(), (size_t)1);
  auto s = suggestions("h.ex", b, h, 3);
  CHECK_EQ(s.size(), (size_t)3);
  auto s2 = suggestions("a.ex", b, h, 5);
  CHECK(!s2.empty() && s2[0].url == "https://a.example/");
  CHECK(suggestions("", b, h).empty());
  unlink(bm.c_str());
  unlink(hist.c_str());
  rmdir(dir.c_str());
  return test_result("web");
}
