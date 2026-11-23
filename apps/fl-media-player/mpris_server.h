// fl-media-player as an MPRIS player (org.mpris.MediaPlayer2 on the session
// bus): the panel's now playing row and the media keys reach it. GDBus runs
// on the fld-glib thread; the handlers are called in the FLTK thread.
#pragma once
#include <functional>
#include <string>

namespace mprisd {

struct Status {
  std::string playback = "Stopped"; // Playing, Paused, Stopped
  std::string track_id;             // an object path naming the item
  std::string title, artist, album, art_url, url;
  double length = -1;   // seconds
  double position = 0;  // seconds
  double volume = 1;    // 0..1
  double rate = 1;
  bool can_next = false, can_prev = false, can_seek = false, shuffle = false;
  std::string loop = "None"; // None, Track, Playlist
};

struct Handlers {
  std::function<void(const std::string &method)> command; // Play, Pause, PlayPause, Stop, Next, Previous
  std::function<void(double seconds)> seek;               // relative
  std::function<void(double seconds)> set_position;       // absolute
  std::function<void(const std::string &uri)> open;
  std::function<void(double)> set_volume;                 // 0..1
  std::function<void(bool)> set_shuffle;
  std::function<void(const std::string &)> set_loop;
  std::function<void()> raise, quit;
};

// Owns org.mpris.MediaPlayer2.fl_media_player.instance<pid>.
void start(const Handlers &h);
// The state changed (FLTK thread): PropertiesChanged for what differs.
void update(const Status &s);
// A jump in the position (seeking).
void seeked(double seconds);
// Keeps logind's idle action (suspend after a while) away while music plays.
void inhibit_idle(bool on);

} // namespace mprisd
