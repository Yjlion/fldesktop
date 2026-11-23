// fl-fonts: fonts as families with their styles and files (unit tested)
#pragma once
#include <map>
#include <string>
#include <vector>

namespace fonts {

struct Face {
  std::string family, style, file;
  int weight = 80;  // fontconfig weight (80 regular, 200 bold)
  int slant = 0;    // 0 roman, 100 italic, 110 oblique
};

struct Family {
  std::string name;
  std::vector<Face> faces; // regular first
  bool user = false;       // all its files are the user's (can be removed)
  bool monospace = false;
};

// Faces grouped into families, sorted by name; a face's family may be a
// list ("DejaVu Sans,DejaVu Sans Condensed"): the first name counts.
std::vector<Family> group(const std::vector<Face> &faces, const std::string &home);
// .ttf .otf .ttc .otc .pfb .pcf(.gz) .woff .woff2
bool is_font_file(const std::string &name);
// under ~/.local/share/fonts or ~/.fonts
bool is_user_font(const std::string &file, const std::string &home);
// where an installed copy goes (~/.local/share/fonts/<name>, made unique)
std::string install_path(const std::string &fonts_dir, const std::string &file);

} // namespace fonts
