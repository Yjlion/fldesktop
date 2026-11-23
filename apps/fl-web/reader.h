// fl-web: reader mode. Finds a page's article (the part with the most
// paragraph text, without menus, sidebars and comments) and lays it out as a
// plain page. No WebKit here (unit tested).
#pragma once
#include <string>

namespace reader {

struct Article {
  std::string title, byline, site;
  std::string html; // the article's content: paragraphs, headings, lists, images...
  int words = 0;
  bool ok() const { return words >= 40; }
};

Article extract(const std::string &page_html, const std::string &url);
// a whole page for the article; dark colors or light, a base font size in px
std::string page(const Article &a, bool dark, int font_px);
// a relative link made absolute against the page's URL
std::string resolve(const std::string &base, const std::string &rel);

} // namespace reader
