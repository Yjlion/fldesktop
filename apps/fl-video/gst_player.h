// fl-video playback engine: GStreamer playbin with a playlist, resume
// positions, track selection, speed, frame stepping and screenshots.
#pragma once
#include <functional>
#include <string>
#include <vector>

#include "frame_view.h"

typedef struct _GstElement GstElement;
typedef struct _GstMessage GstMessage;

class Player {
public:
  Player();
  ~Player();
  bool ok() const { return playbin_ != nullptr; }
  const std::string &init_error() const { return init_error_; }
  fgst::FrameSlot &frames() { return slot_; }
  GstElement *video_sink() const { return vsink_; }

  // playlist
  void load(const std::string &uri_or_path, bool append);
  void play_index(int i);
  void next();
  void prev();
  int count() const { return (int)list_.size(); }
  int current() const { return cur_; }
  struct Item {
    std::string location, title;
    std::string resolved; // the stream URL yt-dlp found for a web page
  };
  const std::vector<Item> &playlist() const { return list_; }

  // transport
  void play();
  void pause();
  void toggle_pause();
  bool paused() const { return paused_; }
  bool at_end() const { return eos_; }
  bool has_media() const { return cur_ >= 0; }
  void seek(double secs, bool exact);
  void seek_relative(double d);
  double position() const;
  double duration() const;
  bool seekable() const { return seekable_; }
  void frame_step(int dir);
  void stop();

  // audio
  void set_volume(double v); // 0 .. 130 (percent, cubic like most players)
  double volume() const { return volume_; }
  void set_mute(bool m);
  bool muted() const { return muted_; }

  void set_rate(double r);
  double rate() const { return rate_; }
  void set_loop(bool l) { loop_ = l; }
  bool loop() const { return loop_; }

  // tracks
  struct Track {
    int index;
    std::string type; // video, audio, sub
    std::string title, lang, codec;
    bool selected;
  };
  std::vector<Track> tracks() const;
  void select_track(const std::string &type, int index);
  void cycle_track(const std::string &type);
  void subtitles_off();
  void set_subtitles_visible(bool v);
  bool subtitles_visible() const;
  bool add_subtitle(const std::string &path);

  bool has_video() const;
  std::string title() const;
  std::string info() const; // codecs and size, for the OSD
  bool screenshot(const std::string &dir, std::string &saved, std::string &err);

  // resume positions (~/.cache/fldesktop/video-positions.ini)
  void set_resume(bool r) { resume_ = r; }
  void save_position();

  // callbacks, in the FLTK thread
  std::function<void()> on_state;    // paused / playing / end changed
  std::function<void()> on_position; // position or duration changed (4x a second)
  std::function<void()> on_media;    // a new item started; title, tracks
  std::function<void()> on_tracks;
  std::function<void(const std::string &)> on_error;
  std::function<void(int)> on_buffering; // percent, 100 = done

private:
  GstElement *playbin_ = nullptr, *vsink_ = nullptr;
  fgst::FrameSlot slot_;
  std::string init_error_;
  std::vector<Item> list_;
  int cur_ = -1;
  bool paused_ = true, eos_ = false, seekable_ = false, loop_ = false, muted_ = false, resume_ = true;
  bool buffering_ = false, want_playing_ = false;
  double volume_ = 100, rate_ = 1.0;
  double pending_seek_ = -1; // resume position once the media is ready
  std::string tag_title_;
  double last_pos_ = -1, last_dur_ = -1;

  void start_item(int i, bool play);
  void set_state_playing(bool playing);
  void handle(GstMessage *m);
  void finish_item(); // EOS: next, loop or stop
  void apply_rate(double pos);
  static void poll_cb(void *);
  void resolve_and_start(int i, bool play);
};
