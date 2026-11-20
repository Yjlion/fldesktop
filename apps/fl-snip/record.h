// fl-snip: screen recording through the ScreenCast portal and GStreamer
#pragma once
#include <cstdint>
#include <string>

namespace record {

struct Options {
  bool audio = false;        // the desktop's sound too
  int seconds = 0;           // stop after this many seconds (0: at Stop)
  std::string output;        // the file (default: Videos/Screencasts/...)
  uint32_t node = 0;         // record this PipeWire node instead of asking the portal
  bool window = true;        // show the time and a Stop button
  bool notify = true;        // a notification when saved
};

// Records until Stop, the time or a signal; prints the file's path.
int run(const Options &opt);

} // namespace record
