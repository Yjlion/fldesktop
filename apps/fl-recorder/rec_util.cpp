#include "rec_util.h"
#include "fld/i18n.h" // N_() only

#include <cmath>
#include <cstdio>

namespace rec {

const std::vector<Format> &formats() {
  static const std::vector<Format> f = {
      {"opus", N_("Opus (small, good quality)"), "opus", "audioconvert ! opusenc bitrate=96000 ! oggmux"},
      {"flac", N_("FLAC (lossless)"), "flac", "audioconvert ! flacenc ! flacparse"},
      {"wav", N_("WAV (uncompressed)"), "wav", "audioconvert ! wavenc"},
      {"mp3", N_("MP3 (plays everywhere)"), "mp3", "audioconvert ! lamemp3enc target=bitrate bitrate=160 ! id3v2mux"},
  };
  return f;
}

const Format *format_by_id(const std::string &id) {
  for (auto &f : formats())
    if (id == f.id) return &f;
  return nullptr;
}

std::string file_name(time_t t, const Format &f) {
  struct tm tm;
  localtime_r(&t, &tm);
  char buf[64];
  strftime(buf, sizeof buf, "Recording %Y-%m-%d %H.%M.%S", &tm);
  return std::string(buf) + "." + f.ext;
}

std::string recordings_dir(const std::string &music_dir) {
  std::string d = music_dir;
  while (d.size() > 1 && d.back() == '/') d.pop_back();
  return d + "/Recordings";
}

static std::string quote(const std::string &s) {
  // a gst-launch string value: in double quotes, \ and " escaped
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

std::string pipeline(const Format &f, const std::string &path, const std::string &source) {
  return source + " ! queue ! audioconvert ! audioresample ! level interval=100000000 post-messages=true ! " +
         f.encoder + " ! filesink location=" + quote(path);
}

double meter(double db) {
  if (!(db > -60)) return 0;
  if (db >= 0) return 1;
  return (db + 60) / 60;
}

std::string duration(double s) {
  if (!(s > 0)) s = 0;
  long t = (long)s;
  char buf[32];
  if (t >= 3600) snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", t / 3600, t / 60 % 60, t % 60);
  else snprintf(buf, sizeof buf, "%ld:%02ld", t / 60, t % 60);
  return buf;
}

std::string renamed(const std::string &old_name, const std::string &typed) {
  std::string t;
  for (char c : typed)
    if (c != '/' && c != '\n' && c != '\r') t += c;
  size_t a = t.find_first_not_of(' '), b = t.find_last_not_of(' ');
  t = a == std::string::npos ? "" : t.substr(a, b - a + 1);
  if (t.empty() || t == "." || t == "..") return "";
  size_t dot = old_name.rfind('.');
  std::string ext = dot == std::string::npos ? "" : old_name.substr(dot);
  if (!ext.empty() && (t.size() < ext.size() || t.compare(t.size() - ext.size(), ext.size(), ext) != 0)) t += ext;
  return t;
}

} // namespace rec
