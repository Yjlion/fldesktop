// fl-docs: flowing text laid out into pages (EPUB, OpenDocument text)
//
// The formats are read into a list of blocks: paragraphs as Pango markup,
// images, rules, tables and page breaks. layout() breaks them into pages of a
// fixed size with Pango, line by line; the pages are drawn with cairo for the
// screen and into a PDF for printing, and searched in their text.
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "backend.h"

namespace docs::flow {

struct Block {
  enum Kind { Text, Image, Rule, PageBreak, Table } kind = Text;
  std::string markup;        // Text: Pango markup
  double size = 0;           // font size in points; 0: the page style's
  std::string family;        // "" or e.g. "Monospace"
  enum Align { Left, Center, Right, Justify } align = Left;
  double indent = 0;         // left indent (points)
  double first_indent = 0;   // extra indent of the first line
  double space_before = 0, space_after = 0;
  std::string bullet;        // drawn left of the first line ("•", "3.")
  std::string image;         // Image: the file's bytes
  double image_w = 0, image_h = 0; // Image: size in points when the document says
  std::vector<std::vector<std::vector<Block>>> rows; // Table: rows of cells of blocks
};

struct PageStyle {
  double w = 432, h = 648;   // points (6 x 9 in)
  double left = 40, right = 40, top = 44, bottom = 44;
  std::string family = "Serif";
  double size = 12;
};

// Escapes text for Pango markup.
std::string escape(const std::string &text);

class Document {
public:
  Document();
  ~Document();
  void layout(const std::vector<Block> &blocks, const PageStyle &style);
  std::vector<PageSize> sizes() const;
  bool render(size_t page, double scale, Bitmap &out);
  std::vector<Hit> find(const std::string &text) const;
  bool write_pdf(const std::string &path, const std::string &title, std::string &err);

private:
  struct Impl;
  std::unique_ptr<Impl> d;
};

// a Backend over a laid out Document
class FlowBackend : public Backend {
public:
  Document doc;
  bool render(size_t page, double scale, Bitmap &out) override { return doc.render(page, scale, out); }
  std::vector<Hit> find(const std::string &text) override { return doc.find(text); }
  bool print_pdf(std::string &path, bool &temporary, std::string &err) override {
    path = temp_pdf_path();
    temporary = true;
    return doc.write_pdf(path, title_, err);
  }
};

} // namespace docs::flow
