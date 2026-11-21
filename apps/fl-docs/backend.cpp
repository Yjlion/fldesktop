// fl-docs: choosing the backend for a file
#include "backend.h"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>

#include "fld/util.h"
#ifdef FLD_DOCS_REFLOW
#include "archive_reader.h"
#endif

namespace docs {

std::string temp_pdf_path() {
  const char *dir = getenv("XDG_RUNTIME_DIR");
  std::string d = dir && *dir ? dir : "/tmp";
  static int n = 0;
  return d + "/fl-docs-print-" + std::to_string(getpid()) + "-" + std::to_string(++n) + ".pdf";
}

std::unique_ptr<Backend> open_document(const std::string &path, std::string &err, const PasswordFn &password) {
  std::string head;
  if (!fld::read_file(path, head, 4096)) {
    err = "The file cannot be read.";
    return nullptr;
  }
  const char *unknown = "This is not a PDF, EPUB, OpenDocument text or comic book file, or it is damaged.";
  Format f = sniff(head, path);
  switch (f) {
  case Format::Pdf: return open_pdf(path, err, password);
#ifdef FLD_DOCS_REFLOW
  case Format::Epub: return open_epub(path, err);
  case Format::Odt: return open_odt(path, err);
  case Format::OdfOther:
    err = "Only OpenDocument text documents can be shown, not spreadsheets, presentations or drawings.";
    return nullptr;
  case Format::Zip:
  case Format::Archive: {
    // an EPUB or ODF whose "mimetype" is not first, or a comic book
    std::vector<std::string> names;
    if (!list_members(path, names, err)) return nullptr;
    for (auto &n : names) {
      if (n == "META-INF/container.xml") return open_epub(path, err);
      if (n == "content.xml") return open_odt(path, err);
    }
    for (auto &n : names)
      if (is_image_name(n)) return open_comic(path, err);
    err = unknown;
    return nullptr;
  }
#else
  case Format::Epub:
  case Format::Odt:
  case Format::OdfOther:
  case Format::Zip:
  case Format::Archive:
    err = "This fl-docs was built for PDF only.";
    return nullptr;
#endif
  case Format::Unknown: break;
  }
  err = unknown;
  return nullptr;
}

} // namespace docs
