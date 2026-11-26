// fl-files: quick look (Space) - the program that previews a file. No FLTK
// (unit tested).
#pragma once
#include <string>
#include <vector>

namespace files {

// fl-view for pictures, fl-docs for documents and e-books, fl-media-player
// for sound and video, fl-edit for text; empty when none fits
std::vector<std::string> preview_command(const std::string &path, const std::string &mime);

} // namespace files
