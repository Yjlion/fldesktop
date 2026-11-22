// fl-docs: Markdown (.md, .markdown, ...)
//
// The text is turned into HTML (markdown.cpp) and laid out like an e-book
// chapter, on A4 pages. Images are read from the disk, relative to the file;
// images on the web are left out (fl-docs does not go online).
#include "backend.h"
#include "flow.h"
#include "fld/util.h"

namespace docs {

std::unique_ptr<Backend> open_markdown(const std::string &path, std::string &err) {
  std::string md;
  if (!fld::read_file(path, md, 32u << 20)) {
    err = "The file cannot be read.";
    return nullptr;
  }
  std::string title;
  std::string html = markdown_to_html(md, &title);
  std::string dir = fld::dirname(path);
  flow::ImageFn image = [&](const std::string &src, std::string &data) {
    std::string s = src.substr(0, src.find_first_of("?#"));
    if (s.compare(0, 7, "file://") == 0) s = s.substr(7);
    else if (s.find("://") != std::string::npos || s.compare(0, 5, "data:") == 0) return false;
    s = fld::uri_decode(s);
    if (s.empty()) return false;
    return fld::read_file(fld::path_join(dir, s), data);
  };
  flow::PageStyle style;
  style.w = 595; // A4
  style.h = 842;
  style.left = style.right = 56;
  style.top = style.bottom = 56;
  style.size = 11;
  std::vector<flow::Block> blocks = flow::html_blocks("<html><body>" + html + "</body></html>", image, style.size);
  auto b = std::make_unique<flow::FlowBackend>();
  b->title_ = title;
  // an empty file is an empty page, as in an editor
  b->doc.begin(std::move(blocks), style);
  b->layout_step(0.1);
  return b;
}

} // namespace docs
