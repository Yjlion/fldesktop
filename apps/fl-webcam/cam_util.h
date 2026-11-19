// fl-webcam helpers without GStreamer or FLTK (unit tested)
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace cam {

// A capture mode of a camera.
struct Mode {
  int w = 0, h = 0;
  double fps = 0;      // highest frame rate for this size and format
  std::string format;  // "raw" or "jpeg"
  bool operator==(const Mode &o) const { return w == o.w && h == o.h && format == o.format; }
};

// Sorts modes (largest first; for one size raw before jpeg when its frame
// rate is as good, otherwise the faster one) and keeps one per size.
std::vector<Mode> best_modes(std::vector<Mode> modes);
// "1280×720 (30 fps)"
std::string mode_label(const Mode &m);
// The mode to start with: the largest one up to 1920x1080 with at least
// 24 fps, else the first.
size_t default_mode(const std::vector<Mode> &modes);

// "Webcam-2026-11-17-10-31-05.jpg" in dir, with -2, -3 ... when it exists
std::string capture_path(const std::string &dir, const std::string &ext, time_t t);
// 75 -> "01:15", 3725 -> "1:02:05"
std::string format_duration(double secs);

} // namespace cam
