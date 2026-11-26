// fl-recorder: formats, file names and the level meter (no FLTK or
// GStreamer; unit tested).
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace rec {

struct Format {
  const char *id, *label, *ext;
  const char *encoder; // GStreamer elements after the level meter
};
const std::vector<Format> &formats(); // Opus (default), FLAC, WAV, MP3 when lame is there
const Format *format_by_id(const std::string &id);

// "Recording 2026-11-25 14.03.22.opus"
std::string file_name(time_t t, const Format &f);
// the folder for recordings: $XDG_MUSIC_DIR/Recordings (music_dir given)
std::string recordings_dir(const std::string &music_dir);
// the gst-launch description of the pipeline (source -> level -> encoder -> file)
std::string pipeline(const Format &f, const std::string &path, const std::string &source = "autoaudiosrc");
// a level message's peak in dB (-inf..0) as 0..1 for a meter (-60 dB = 0)
double meter(double db);
// "1:05", "1:02:03"
std::string duration(double seconds);
// a file name typed by the user: no "/", not empty, keeps the extension
std::string renamed(const std::string &old_name, const std::string &typed);

} // namespace rec
