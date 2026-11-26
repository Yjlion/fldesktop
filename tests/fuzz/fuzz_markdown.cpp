#include "docs_util.h"
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  std::string title;
  docs::markdown_to_html(std::string(reinterpret_cast<const char *>(data), size), &title);
  return 0;
}
