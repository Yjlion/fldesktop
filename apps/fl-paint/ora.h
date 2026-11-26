// fl-paint: OpenRaster (.ora) files, the layered format of MyPaint, Krita and
// GIMP: a zip with mimetype, stack.xml, the layers as PNG and a merged image.
// The PNG coding is the caller's (FLTK); no FLTK here (unit tested).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ora {

// a zip file's entries (stored or deflated)
struct ZipEntry {
  std::string name, data;
};
// the archive with each entry stored (PNG data is compressed already); the
// first entry should be "mimetype"
std::string zip_write(const std::vector<ZipEntry> &entries);
// reads stored and deflated entries; false when it is not a zip
bool zip_read(const std::string &zip, std::vector<ZipEntry> &out, size_t max_total = 512u << 20);

struct LayerInfo {
  std::string name, src; // src: the PNG's path in the archive
  int x = 0, y = 0;
  double opacity = 1.0;
  bool visible = true;
};
// stack.xml: layers top first, as the format has them
std::string stack_xml(int w, int h, const std::vector<LayerInfo> &top_first);
bool parse_stack(const std::string &xml, int &w, int &h, std::vector<LayerInfo> &top_first);

uint32_t crc32(const std::string &data);

} // namespace ora
