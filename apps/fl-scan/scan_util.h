// fl-scan: SANE through scanimage (unit tested parsing and arguments)
#pragma once
#include <string>
#include <vector>

namespace scan {

struct Device {
  std::string name;  // airscan:e0:Canon MF240
  std::string label; // "Canon MF240 (ip=...)"
};
// scanimage -L: "device `airscan:e0:X' is a eSCL Canon X ip=192.168.1.5"
std::vector<Device> parse_devices(const std::string &out);

struct Options {
  std::vector<std::string> modes;    // Color, Gray, Lineart
  std::vector<int> resolutions;      // dpi
  std::vector<std::string> sources;  // Flatbed, ADF, ADF Duplex
  std::string mode, source;          // the defaults
  int resolution = 0;
  double max_w = 0, max_h = 0;       // mm (the -x / -y ranges)
};
// scanimage -d DEV -A
Options parse_options(const std::string &out);

struct Request {
  std::string device, mode, source;
  int resolution = 150;
  // the area in mm; w or h 0 = everything
  double x = 0, y = 0, w = 0, h = 0;
  std::string output; // a .png file, or a batch pattern with %d
  bool batch = false; // the whole feeder (ADF)
};
std::vector<std::string> scanimage_args(const Request &r);

// mm at a resolution to pixels and back; a page's size in points (1/72 in)
int mm_to_px(double mm, int dpi);
double px_to_pt(int px, int dpi);
// "scanimage: open of device x failed: Device busy" -> "Device busy"
std::string error_text(const std::string &out);

} // namespace scan
