// fl-media-player playback engine: GStreamer playbin with a playlist (play
// order with shuffle and repeat), resume positions, track selection, speed,
// frame stepping, screenshots, and the tags and cover art of music.
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "frame_view.h"
#include "playlist_util.h"

typedef struct _GstElement GstElement;
typedef struct _GstMessage GstMessage;

class Player {
public:
  Player();
  ~Player();
  // starts GStreamer (done by the first item played); false with init_error()
  bool ensure_engine();
  bool ok() const { return playbin_ != nullptr; }
  const std::string &init_error() const { return init_error_; }
  fgst::FrameSlot &frames() { return slot_; }
  GstElement *video_sink() const { return vsink_; }
  // software rendering: frames scaled to fit w x h (0, 0: as they are);
  // kept for an engine started later
  void set_scale_to(int w, int h);

  // playlist
  // a media file, a URL, or a playlist file (.m3u, .pls: its entries)
  void load(const std::string &uri_or_path, bool append);
  void play_index(int i);
  void next();
  void prev();
  bool has_next() const;
  bool has_prev() const;
  int count() const { return (int)list_.size(); }
  int current() const { return cur_; }
  struct Item {
    std::string location, title;
    std::string resolved; // the stream URL yt-dlp found for a web page
    double length = -1;   // from a playlist file, or once played
  };
  const std::vector<Item> &playlist() const { return list_; }
  // editing (the current item keeps playing)
  void remove(int i);
  void move(int from, int to);
  void clear();
  bool save_playlist(const std::string &path, std::string &err) const; // .m3u or .pls
  void set_shuffle(bool s);
  bool shuffle() const { return shuffle_; }
  void set_repeat(media::Repeat r) {
    repeat_ = r;
    prepare_gapless();
  }
  media::Repeat repeat() const { return repeat_; }
  // the current item's tags (music), and a file with its cover art ("" if none)
  const std::string &artist() const { return tag_artist_; }
  const std::string &album() const { return tag_album_; }
  const std::string &track_title() const { return tag_track_; }
  const std::string &cover_path() const { return cover_path_; }

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
  void set_loop(bool l) { set_repeat(l ? media::Repeat::One : media::Repeat::Off); }
  // the next local item is queued before this one ends: no gap between
  // tracks (on by default)
  void set_gapless(bool g) {
    gapless_ = g;
    prepare_gapless();
  }
  bool gapless() const { return gapless_; }
  int gapless_switches() const { return gapless_switches_; } // items started without a gap
  bool loop() const { return repeat_ == media::Repeat::One; }

  // tracks
  struct Track {
    int index;
    std::string type; // video, audio, sub
    std::string title, lang, codec;
    bool selected;
  };
  std::vector<Track> tracks() const;

  // chapters of the current item (from its table of contents): start, title
  struct Chapter {
    double start;
    std::string title;
  };
  const std::vector<Chapter> &chapters() const { return chapters_; }

  // the sound: volume levelling from ReplayGain tags, and a 10-band
  // equalizer (gains in dB, -24 .. +12; 31 Hz .. 16 kHz). The ReplayGain
  // switch takes effect from the next item.
  void set_replaygain(bool on);
  bool replaygain() const { return replaygain_; }
  bool has_equalizer() const { return eq_ != nullptr; }
  void set_equalizer(const std::vector<double> &gains);
  const std::vector<double> &equalizer() const { return eq_gains_; }
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
  std::function<void()> on_playlist;     // items added, removed or moved
  std::function<void()> on_tags;         // artist, album or cover changed

private:
  GstElement *playbin_ = nullptr, *vsink_ = nullptr;
  GstElement *eq_ = nullptr;            // in the audio filter bin
  bool replaygain_ = false, filters_dirty_ = true;
  std::vector<double> eq_gains_ = std::vector<double>(10, 0.0);
  std::vector<Chapter> chapters_;
  void build_filters(); // scaletempo ! [rgvolume !] equalizer-10bands, in READY or NULL
  fgst::FrameSlot slot_;
  std::string init_error_;
  std::vector<Item> list_;
  int cur_ = -1;
  bool paused_ = true, eos_ = false, seekable_ = false, muted_ = false, resume_ = true, shuffle_ = false;
  media::Repeat repeat_ = media::Repeat::Off;
  std::vector<int> order_; // the play order (indices into list_)
  std::string tag_artist_, tag_album_, tag_track_, cover_path_;
  bool cover_from_tags_ = false;
  bool buffering_ = false, want_playing_ = false;
  double volume_ = 100, rate_ = 1.0;
  double pending_seek_ = -1; // resume position once the media is ready
  std::string tag_title_;
  double last_pos_ = -1, last_dur_ = -1;
  int scale_w_ = 0, scale_h_ = 0;

  void start_item(int i, bool play);
  void set_state_playing(bool playing);
  void handle(GstMessage *m);
  void finish_item(); // EOS: next, loop or stop
  void apply_rate(double pos);
  static void poll_cb(void *);
  void resolve_and_start(int i, bool play);
  void rebuild_order();
  void append(const Item &it);
  void set_cover(const std::string &bytes, const std::string &mime, bool from_tags);
  void find_folder_cover();

  // gapless playback: the UI thread keeps the next item's URI ready, the
  // streaming thread (about-to-finish) queues it, STREAM_START switches over
  bool gapless_ = true;
  std::mutex gapless_mu_;
  std::string gapless_uri_;
  int gapless_index_ = -1;
  std::atomic<int> gapless_queued_{-1};
  int gapless_switches_ = 0;
  void prepare_gapless();
  void gapless_started(int index);
  static void about_to_finish(GstElement *, void *self);
};
