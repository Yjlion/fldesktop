// fl-docs: a document format behind one interface
//
// The viewer only needs the page sizes, a page as pixels, the places a text
// appears and a PDF to print. PDF goes through poppler; EPUB and OpenDocument
// text are laid out into pages with Pango (flow.h); comic books are a page per
// image.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "docs_util.h"

namespace docs {

struct Hit {
  int page;
  double x, y, w, h; // points, top-left origin
};

struct Bitmap {
  int w = 0, h = 0;
  std::vector<unsigned char> rgb; // w * h * 3, on white paper
};

class Backend {
public:
  virtual ~Backend() = default;
  const std::vector<PageSize> &sizes() const { return sizes_; }
  const std::string &title() const { return title_; }
  // a page at `scale` pixels per point
  virtual bool render(size_t page, double scale, Bitmap &out) = 0;
  // every place the text appears (case-insensitive), page by page
  virtual std::vector<Hit> find(const std::string &text) = 0;
  virtual bool has_text() const { return true; }
  // A long book is laid out into pages a step at a time, so that its first
  // pages show at once: lays out more for about `seconds` (sizes() grows;
  // its last page may still change) and returns true when all of it is.
  virtual bool layout_step(double seconds) { return true; }
  virtual bool laid_out() const { return true; }
  void finish_layout() {
    while (!layout_step(1e9)) {}
  }
  // A PDF of the document for printing: the file itself, or one written to
  // `path` (then `temporary` is set and the caller removes it).
  virtual bool print_pdf(std::string &path, bool &temporary, std::string &err) = 0;

  // filled in by the backends
  std::vector<PageSize> sizes_;
  std::string title_;
};

// Asks for a password (false: cancelled).
using PasswordFn = std::function<bool(std::string &password)>;

// Opens a PDF, EPUB, OpenDocument text or comic book (.cbz; .cbr, .cb7 and
// .cbt too). `password` is asked when a PDF is locked; null: fail instead.
std::unique_ptr<Backend> open_document(const std::string &path, std::string &err, const PasswordFn &password);

// the backends (backend_*.cpp)
std::unique_ptr<Backend> open_pdf(const std::string &path, std::string &err, const PasswordFn &password);
std::unique_ptr<Backend> open_comic(const std::string &path, std::string &err);
std::unique_ptr<Backend> open_epub(const std::string &path, std::string &err);
std::unique_ptr<Backend> open_odt(const std::string &path, std::string &err);

// a temporary file name for print_pdf
std::string temp_pdf_path();

} // namespace docs
