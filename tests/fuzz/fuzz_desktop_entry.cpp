#include <fld/desktop_entry.h>
#include <fld/mime.h>
#include <unistd.h>
#include "fuzz_util.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  std::string path;
  int fd = fuzz_memfd(data, size, path);
  if (fd < 0) return 0;
  fld::DesktopEntry e;
  if (e.load(path, "fuzz.desktop")) {
    e.visible();
    e.command({"/tmp/a b", "file:///x%20y"});
    e.main_category();
  }
  fld::desktop_unescape(std::string(reinterpret_cast<const char *>(data), size));
  // content sniffing (shared-mime-info magic)
  fld::mime_type(path);
  close(fd);
  return 0;
}
