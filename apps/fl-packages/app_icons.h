// fl-packages: application names and icons for packages, from the system's
// AppStream catalog (/usr/share/swcatalog, /var/lib/app-info ...).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace appicons {

struct AppInfo {
  std::string name;      // the application's name ("Firefox")
  std::string icon_file; // a cached / local icon file (png, svg, jxl), best near 64 px
  std::string stock;     // an icon theme name
};

// pkgname -> the first application in it. Slow (reads the whole catalog):
// call it in a thread. Empty (with err) without AppStream data.
std::map<std::string, AppInfo> load_catalog(std::string *err = nullptr);

// Decodes an image file to RGBA (8 bit, rows packed). PNG and SVG are left
// to FLTK; this handles JPEG XL (Arch's catalog), when built with libjxl.
bool decode_jxl(const std::string &path, int &w, int &h, std::vector<uint8_t> &rgba);
bool have_jxl();

} // namespace appicons
