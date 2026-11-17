// Helpers for fl-video (no FLTK or mpv dependency)
#pragma once
#include <string>
#include <vector>

namespace video {

// 75 -> "1:15", 3725 -> "1:02:05"; negative/NaN -> "--:--"
std::string format_time(double seconds);
// Turns command line / drag-and-drop text into things mpv can load: file://
// URIs are decoded, other URLs (http, rtsp, ...) are kept, relative paths are
// made absolute. Empty lines and comments (#) are skipped.
std::vector<std::string> parse_uri_list(const std::string &text, const std::string &cwd);
bool is_url(const std::string &s);
// True for file names that look like subtitles (srt, ass, ssa, vtt, sub)
bool is_subtitle(const std::string &path);
// Playback speeds offered in the menu, and the next one up / down.
const std::vector<double> &speeds();
double next_speed(double cur, int dir);

} // namespace video
