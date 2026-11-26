#include "preview_util.h"

namespace files {

static bool starts(const std::string &s, const char *p) { return s.compare(0, std::char_traits<char>::length(p), p) == 0; }

std::vector<std::string> preview_command(const std::string &path, const std::string &mime) {
  if (path.empty()) return {};
  if (starts(mime, "image/")) return {"fl-view", path};
  if (mime == "application/pdf" || mime == "application/epub+zip" || mime == "text/markdown" ||
      mime == "application/vnd.oasis.opendocument.text" || mime == "application/vnd.comicbook+zip" ||
      mime == "application/x-cbz")
    return {"fl-docs", path};
  if (starts(mime, "audio/") || starts(mime, "video/")) return {"fl-media-player", path};
  if (starts(mime, "text/") || mime == "application/json" || mime == "application/xml" ||
      mime == "application/x-shellscript" || mime == "application/javascript")
    return {"fl-edit", path};
  return {};
}

} // namespace files
