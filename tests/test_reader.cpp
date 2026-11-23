// fl-web: reader mode
#include "reader.h"
#include "test.h"

using namespace reader;

static std::string para(int i) {
  return "<p>This is paragraph " + std::to_string(i) +
         " of the story, with enough words in it to count, and a comma or two, so that it scores well.</p>";
}

int main() {
  CHECK_EQ(resolve("https://a.org/b/c.html?x=1", "d.png"), std::string("https://a.org/b/d.png"));
  CHECK_EQ(resolve("https://a.org/b/c.html", "../d.png"), std::string("https://a.org/d.png"));
  CHECK_EQ(resolve("https://a.org/b/c.html", "/e"), std::string("https://a.org/e"));
  CHECK_EQ(resolve("https://a.org/b/c.html", "//cdn.x/i.png"), std::string("https://cdn.x/i.png"));
  CHECK_EQ(resolve("https://a.org/b/c.html", "#top"), std::string("https://a.org/b/c.html#top"));
  CHECK_EQ(resolve("https://a.org/b/", "./f/g?q=1"), std::string("https://a.org/b/f/g?q=1"));
  CHECK_EQ(resolve("https://a.org", "x"), std::string("https://a.org/x"));
  CHECK_EQ(resolve("https://a.org/b", "mailto:x@y"), std::string("mailto:x@y"));

  std::string body;
  for (int i = 0; i < 6; i++) body += para(i);
  std::string html =
      "<html><head><title>Ignored</title><meta property=\"og:title\" content=\"The Big Story\">"
      "<meta property=\"og:site_name\" content=\"Daily\"><meta name=\"author\" content=\"A. Writer\">"
      "<script>var x = '<p>no</p>';</script></head><body>"
      "<nav><a href=\"/\">Home</a> <a href=\"/news\">News</a></nav>"
      "<div class=\"sidebar\"><p>Sidebar text that is long enough to count as a paragraph, really, it is.</p></div>"
      "<article><h1>The Big Story</h1><div class=\"share-buttons\">Share this</div>" +
      body +
      "<p>See <a href=\"/more\" onclick=\"x()\">more</a> and <a href=\"javascript:evil()\">this</a>.</p>"
      "<figure><img src=\"img/a.jpg\" alt=\"A picture\" width=\"10\"><figcaption>Caption</figcaption></figure>"
      "<pre>  code   kept\n  as is</pre></article>"
      "<div id=\"comments\"><p>A comment that is also long enough to count as a paragraph, yes.</p></div>"
      "<footer>Footer</footer></body></html>";
  Article a = extract(html, "https://news.example/2026/story.html");
  CHECK_EQ(a.title, std::string("The Big Story"));
  CHECK_EQ(a.site, std::string("Daily"));
  CHECK_EQ(a.byline, std::string("A. Writer"));
  CHECK(a.ok());
  CHECK(a.html.find("paragraph 5") != std::string::npos);
  CHECK(a.html.find("Sidebar") == std::string::npos);
  CHECK(a.html.find("comment") == std::string::npos);
  CHECK(a.html.find("Share this") == std::string::npos);
  CHECK(a.html.find("Home") == std::string::npos);
  CHECK(a.html.find("<h1>") == std::string::npos); // the title is not repeated
  CHECK(a.html.find("href=\"https://news.example/more\"") != std::string::npos);
  CHECK(a.html.find("onclick") == std::string::npos);
  CHECK(a.html.find("javascript:") == std::string::npos);
  CHECK(a.html.find("<img src=\"https://news.example/2026/img/a.jpg\" alt=\"A picture\">") != std::string::npos);
  CHECK(a.html.find("width") == std::string::npos);
  CHECK(a.html.find("  code   kept\n  as is") != std::string::npos);
  CHECK(a.html.find("<script") == std::string::npos);

  std::string p = page(a, true, 19);
  CHECK(p.find("<title>The Big Story</title>") != std::string::npos);
  CHECK(p.find("font-size:19px") != std::string::npos);
  CHECK(p.find("Daily · A. Writer") != std::string::npos);
  CHECK(p.find("#1e1f22") != std::string::npos);

  // the title from <title>, then <h1>; escaping
  Article b = extract("<html><head><title>T &amp; U</title></head><body><div>" + body + "</div></body></html>", "http://x/");
  CHECK_EQ(b.title, std::string("T & U"));
  CHECK(page(b, false, 18).find("<title>T &amp; U</title>") != std::string::npos);
  Article c = extract("<body><h1>Only heading</h1><p>short</p></body>", "http://x/");
  CHECK_EQ(c.title, std::string("Only heading"));
  CHECK(!c.ok());
  CHECK(!extract("", "http://x/").ok());
  return test_result("reader");
}
