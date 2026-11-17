// Helpers for fl-video (no FLTK or GStreamer dependency)
#pragma once
#include <string>
#include <vector>

namespace fld { class Ini; }

namespace video {

// 75 -> "1:15", 3725 -> "1:02:05"; negative/NaN -> "--:--"
std::string format_time(double seconds);
// Turns command line / drag-and-drop text into things fl-video can load: file://
// URIs are decoded, other URLs (http, rtsp, ...) are kept, relative paths are
// made absolute. Empty lines and comments (#) are skipped.
std::vector<std::string> parse_uri_list(const std::string &text, const std::string &cwd);
bool is_url(const std::string &s);
// True for file names that look like subtitles (srt, ass, ssa, vtt, sub)
bool is_subtitle(const std::string &path);
// A subtitle file next to a local video with the same base name
// (film.mkv -> film.srt, film.en.srt, ...), or "".
std::string find_subtitle(const std::string &video_path);
// Playback speeds offered in the menu, and the next one up / down.
const std::vector<double> &speeds();
double next_speed(double cur, int dir);

// A local path or URL as a URI for GStreamer: paths become file:// URIs with
// percent-encoding, URLs are kept.
std::string to_uri(const std::string &s);
// Whether a URL most likely is a web page rather than a media stream (a video
// site), so it should go through yt-dlp first.
bool needs_resolver(const std::string &url);

// Resume positions: one [key] group per file with Position and Name.
std::string resume_key(const std::string &location);
// The position to resume from, or 0 (none, too close to the start / end).
double resume_get(const fld::Ini &ini, const std::string &location);
// Stores the position; positions near the start or the end remove the entry.
void resume_put(fld::Ini &ini, const std::string &location, double pos, double duration);

} // namespace video
