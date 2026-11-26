// fl-files: the program Space previews a file with
#include "preview_util.h"
#include "test.h"

int main() {
  using files::preview_command;
  CHECK_EQ(preview_command("/a.png", "image/png")[0], std::string("fl-view"));
  CHECK_EQ(preview_command("/a.pdf", "application/pdf")[0], std::string("fl-docs"));
  CHECK_EQ(preview_command("/a.epub", "application/epub+zip")[0], std::string("fl-docs"));
  CHECK_EQ(preview_command("/a.ogg", "audio/ogg")[0], std::string("fl-media-player"));
  CHECK_EQ(preview_command("/a.mkv", "video/x-matroska")[1], std::string("/a.mkv"));
  CHECK_EQ(preview_command("/a.txt", "text/plain")[0], std::string("fl-edit"));
  CHECK(preview_command("/a.bin", "application/octet-stream").empty());
  CHECK(preview_command("", "image/png").empty());
  return test_result("files-preview");
}
