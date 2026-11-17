#include "gst_player.h"

#include <FL/Fl.H>
#include <gst/audio/streamvolume.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <gst/tag/tag.h>
#include <gst/video/video.h>

#include <cmath>
#include <cstring>
#include <ctime>
#include <thread>

#include "fld/ini.h"
#include "fld/util.h"
#include "video_util.h"

// playbin's flags (GstPlayFlags is not in a public header)
enum { PLAY_FLAG_VIDEO = 1 << 0, PLAY_FLAG_AUDIO = 1 << 1, PLAY_FLAG_TEXT = 1 << 2 };

static std::string positions_path() { return fld::cache_home() + "/fldesktop/video-positions.ini"; }

Player::Player() {
  if (!fgst::init(&init_error_)) return;
  playbin_ = gst_element_factory_make("playbin", "player");
  if (!playbin_) {
    init_error_ = "The GStreamer \"playbin\" element is missing (install the GStreamer base plugins).";
    return;
  }
  gst_object_ref_sink(playbin_);
  vsink_ = fgst::make_sink(&slot_);
  if (vsink_) g_object_set(playbin_, "video-sink", vsink_, NULL);
  // keep the pitch when playing faster or slower
  if (GstElement *st = gst_element_factory_make("scaletempo", nullptr)) g_object_set(playbin_, "audio-filter", st, NULL);
  if (getenv("FLD_VIDEO_DEBUG")) gst_debug_set_threshold_from_string("*:2", TRUE);
  Fl::add_timeout(0.05, poll_cb, this);
}

Player::~Player() {
  Fl::remove_timeout(poll_cb, this);
  if (playbin_) {
    gst_element_set_state(playbin_, GST_STATE_NULL);
    gst_object_unref(playbin_);
  }
}

// ---- playlist ---------------------------------------------------------------------------

void Player::load(const std::string &loc, bool append) {
  Item it;
  it.location = loc;
  if (!append) {
    save_position();
    list_.clear();
    cur_ = -1;
  }
  list_.push_back(it);
  if (!append || cur_ < 0) start_item((int)list_.size() - 1, true);
  else if (on_media) on_media();
}

void Player::play_index(int i) {
  if (i < 0 || i >= (int)list_.size()) return;
  save_position();
  start_item(i, true);
}

void Player::next() {
  if (cur_ + 1 < (int)list_.size()) play_index(cur_ + 1);
}

void Player::prev() {
  // like most players: back to the start first, then the previous item
  if (position() > 3 || cur_ == 0) seek(0, false);
  else play_index(cur_ - 1);
}

void Player::start_item(int i, bool play) {
  const Item &item = list_[(size_t)i];
  if (item.resolved.empty() && video::needs_resolver(item.location) && !fld::which("yt-dlp").empty()) {
    resolve_and_start(i, play);
    return;
  }
  cur_ = i;
  eos_ = false;
  seekable_ = false;
  tag_title_.clear();
  last_pos_ = last_dur_ = -1;
  rate_ = 1.0;
  slot_.clear();
  gst_element_set_state(playbin_, GST_STATE_READY);
  std::string uri = item.resolved.empty() ? video::to_uri(item.location) : item.resolved;
  std::string sub = item.resolved.empty() ? video::find_subtitle(item.location) : "";
  g_object_set(playbin_, "uri", uri.c_str(), "suburi", sub.empty() ? nullptr : video::to_uri(sub).c_str(), NULL);
  int flags = 0;
  g_object_get(playbin_, "flags", &flags, NULL);
  g_object_set(playbin_, "flags", flags | PLAY_FLAG_VIDEO | PLAY_FLAG_AUDIO | PLAY_FLAG_TEXT, NULL);
  pending_seek_ = -1;
  if (resume_) {
    fld::Ini ini;
    ini.load(positions_path());
    double p = video::resume_get(ini, item.location);
    if (p > 0) pending_seek_ = p;
  }
  set_volume(volume_);
  set_mute(muted_);
  set_state_playing(play);
  if (on_media) on_media();
}

// Web pages of video sites go through yt-dlp (in a thread) for a stream URL.
void Player::resolve_and_start(int i, bool play) {
  std::string loc = list_[(size_t)i].location;
  struct Job {
    Player *p;
    int index;
    bool play;
    std::string loc, url, title, err;
  };
  Job *job = new Job{this, i, play, loc, "", "", ""};
  gst_element_set_state(playbin_, GST_STATE_READY);
  slot_.clear();
  cur_ = i;
  if (on_media) on_media();
  std::thread([job] {
    std::string out;
    int rc = fld::run_capture({"yt-dlp", "--no-playlist", "-f", "b", "--get-url", "--get-title", job->loc}, out);
    std::vector<std::string> lines = fld::split(fld::trim(out), '\n');
    if (rc == 0 && lines.size() >= 2) {
      job->title = lines[0];
      job->url = lines.back();
    } else {
      job->err = lines.empty() ? "yt-dlp could not find a stream" : lines.back();
    }
    Fl::awake([](void *d) {
      Job *j = static_cast<Job *>(d);
      Player *p = j->p;
      if (j->index < (int)p->list_.size() && p->list_[(size_t)j->index].location == j->loc) {
        if (!j->url.empty()) {
          p->list_[(size_t)j->index].resolved = j->url;
          if (!j->title.empty()) p->list_[(size_t)j->index].title = j->title;
          p->start_item(j->index, j->play);
        } else if (p->on_error) {
          p->on_error(j->err);
        }
      }
      delete j;
    }, job);
  }).detach();
}

// ---- transport --------------------------------------------------------------------------

void Player::set_state_playing(bool playing) {
  want_playing_ = playing;
  paused_ = !playing;
  if (buffering_ && playing) {
    gst_element_set_state(playbin_, GST_STATE_PAUSED);
  } else {
    GstStateChangeReturn r = gst_element_set_state(playbin_, playing ? GST_STATE_PLAYING : GST_STATE_PAUSED);
    if (r == GST_STATE_CHANGE_FAILURE && on_error) on_error("The file could not be played.");
  }
  if (on_state) on_state();
}

void Player::play() {
  if (cur_ < 0) return;
  if (eos_) {
    eos_ = false;
    seek(0, false);
  }
  set_state_playing(true);
}

void Player::pause() { set_state_playing(false); }

void Player::toggle_pause() {
  if (paused_ || eos_) play();
  else pause();
}

void Player::stop() {
  save_position();
  gst_element_set_state(playbin_, GST_STATE_NULL);
  cur_ = -1;
  paused_ = true;
  slot_.clear();
  if (on_state) on_state();
}

double Player::position() const {
  gint64 p = 0;
  if (!playbin_ || !gst_element_query_position(playbin_, GST_FORMAT_TIME, &p)) return last_pos_;
  return p / (double)GST_SECOND;
}

double Player::duration() const {
  gint64 d = 0;
  if (!playbin_ || !gst_element_query_duration(playbin_, GST_FORMAT_TIME, &d) || d <= 0) return -1;
  return d / (double)GST_SECOND;
}

void Player::seek(double secs, bool exact) {
  if (cur_ < 0) return;
  if (secs < 0) secs = 0;
  double d = duration();
  if (d > 0 && secs > d) secs = d - 0.1;
  eos_ = false;
  int flags = GST_SEEK_FLAG_FLUSH | (exact ? GST_SEEK_FLAG_ACCURATE : GST_SEEK_FLAG_KEY_UNIT | GST_SEEK_FLAG_SNAP_NEAREST);
  gst_element_seek(playbin_, rate_, GST_FORMAT_TIME, (GstSeekFlags)flags, GST_SEEK_TYPE_SET,
                   (gint64)(secs * GST_SECOND), GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
  last_pos_ = secs;
  if (on_position) on_position();
}

void Player::seek_relative(double d) {
  double p = position();
  if (p < 0) return;
  seek(p + d, false);
}

void Player::apply_rate(double pos) {
  gst_element_seek(playbin_, rate_, GST_FORMAT_TIME, (GstSeekFlags)(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE),
                   GST_SEEK_TYPE_SET, (gint64)(pos * GST_SECOND), GST_SEEK_TYPE_NONE, GST_CLOCK_TIME_NONE);
}

void Player::set_rate(double r) {
  if (r <= 0) return;
  rate_ = r;
  double p = position();
  if (cur_ >= 0 && p >= 0) apply_rate(p);
}

void Player::frame_step(int dir) {
  if (cur_ < 0) return;
  if (!paused_) pause();
  if (dir > 0) {
    if (vsink_) gst_element_send_event(playbin_, gst_event_new_step(GST_FORMAT_BUFFERS, 1, 1.0, TRUE, FALSE));
  } else {
    // there is no stepping backwards: seek exactly one frame back
    double fps = 25;
    GstPad *pad = vsink_ ? gst_element_get_static_pad(vsink_, "sink") : nullptr;
    if (pad) {
      if (GstCaps *caps = gst_pad_get_current_caps(pad)) {
        GstVideoInfo vi;
        if (gst_video_info_from_caps(&vi, caps) && vi.fps_n > 0 && vi.fps_d > 0) fps = (double)vi.fps_n / vi.fps_d;
        gst_caps_unref(caps);
      }
      gst_object_unref(pad);
    }
    double p = position();
    if (p > 0) seek(std::max(0.0, p - 1.0 / fps), true);
  }
}

// ---- audio ------------------------------------------------------------------------------

void Player::set_volume(double v) {
  volume_ = std::max(0.0, std::min(130.0, v));
  if (playbin_)
    gst_stream_volume_set_volume(GST_STREAM_VOLUME(playbin_), GST_STREAM_VOLUME_FORMAT_CUBIC, volume_ / 100.0);
}

void Player::set_mute(bool m) {
  muted_ = m;
  if (playbin_) g_object_set(playbin_, "mute", m ? TRUE : FALSE, NULL);
}

// ---- tracks -----------------------------------------------------------------------------

static std::string tag_string(GstTagList *tags, const char *tag) {
  gchar *s = nullptr;
  std::string r;
  if (tags && gst_tag_list_get_string(tags, tag, &s) && s) r = s;
  g_free(s);
  return r;
}

std::vector<Player::Track> Player::tracks() const {
  std::vector<Track> v;
  if (!playbin_ || cur_ < 0) return v;
  struct Kind {
    const char *type, *n_prop, *cur_prop, *signal;
  } kinds[] = {{"video", "n-video", "current-video", "get-video-tags"},
               {"audio", "n-audio", "current-audio", "get-audio-tags"},
               {"sub", "n-text", "current-text", "get-text-tags"}};
  int flags = 0;
  g_object_get(playbin_, "flags", &flags, NULL);
  for (auto &k : kinds) {
    int n = 0, current = -1;
    g_object_get(playbin_, k.n_prop, &n, k.cur_prop, &current, NULL);
    for (int i = 0; i < n; i++) {
      GstTagList *tags = nullptr;
      g_signal_emit_by_name(playbin_, k.signal, i, &tags);
      Track t;
      t.index = i;
      t.type = k.type;
      t.title = tag_string(tags, GST_TAG_TITLE);
      std::string lang = tag_string(tags, GST_TAG_LANGUAGE_CODE);
      if (lang.empty()) lang = tag_string(tags, GST_TAG_LANGUAGE_NAME);
      if (!lang.empty()) {
        const gchar *name = gst_tag_get_language_name(lang.c_str());
        t.lang = name ? name : lang;
      }
      t.codec = tag_string(tags, !strcmp(k.type, "video") ? GST_TAG_VIDEO_CODEC
                                  : !strcmp(k.type, "audio") ? GST_TAG_AUDIO_CODEC
                                                             : GST_TAG_SUBTITLE_CODEC);
      t.selected = i == current && (strcmp(k.type, "sub") || (flags & PLAY_FLAG_TEXT));
      if (tags) gst_tag_list_unref(tags);
      v.push_back(t);
    }
  }
  return v;
}

void Player::select_track(const std::string &type, int index) {
  const char *prop = type == "video" ? "current-video" : type == "audio" ? "current-audio" : "current-text";
  if (type == "sub") set_subtitles_visible(true);
  g_object_set(playbin_, prop, index, NULL);
  if (on_tracks) on_tracks();
}

void Player::cycle_track(const std::string &type) {
  const char *n_prop = type == "video" ? "n-video" : type == "audio" ? "n-audio" : "n-text";
  const char *cur_prop = type == "video" ? "current-video" : type == "audio" ? "current-audio" : "current-text";
  int n = 0, cur = 0;
  g_object_get(playbin_, n_prop, &n, cur_prop, &cur, NULL);
  if (n <= 0) return;
  if (type == "sub") {
    // off -> 0 -> 1 -> ... -> off
    if (!subtitles_visible()) {
      select_track("sub", 0);
    } else if (cur + 1 >= n) {
      subtitles_off();
    } else {
      select_track("sub", cur + 1);
    }
    return;
  }
  select_track(type, (cur + 1) % n);
}

void Player::subtitles_off() { set_subtitles_visible(false); }

void Player::set_subtitles_visible(bool v) {
  int flags = 0;
  g_object_get(playbin_, "flags", &flags, NULL);
  flags = v ? (flags | PLAY_FLAG_TEXT) : (flags & ~PLAY_FLAG_TEXT);
  g_object_set(playbin_, "flags", flags, NULL);
  if (on_tracks) on_tracks();
}

bool Player::subtitles_visible() const {
  int flags = 0;
  g_object_get(playbin_, "flags", &flags, NULL);
  return flags & PLAY_FLAG_TEXT;
}

// playbin reads an external subtitle file only when the pipeline starts:
// restart at the same position with it.
bool Player::add_subtitle(const std::string &path) {
  if (cur_ < 0) return false;
  double pos = position();
  bool was_playing = !paused_;
  gst_element_set_state(playbin_, GST_STATE_READY);
  std::string uri = video::to_uri(path);
  g_object_set(playbin_, "suburi", uri.c_str(), NULL);
  set_subtitles_visible(true);
  pending_seek_ = pos > 0 ? pos : -1;
  set_state_playing(was_playing);
  return true;
}

bool Player::has_video() const {
  if (!playbin_ || cur_ < 0) return false;
  int n = 0;
  g_object_get(playbin_, "n-video", &n, NULL);
  return n > 0;
}

std::string Player::title() const {
  if (cur_ < 0) return "";
  const Item &it = list_[(size_t)cur_];
  if (!tag_title_.empty()) return tag_title_;
  if (!it.title.empty()) return it.title;
  if (video::is_url(it.location)) return it.location;
  return fld::basename(it.location);
}

std::string Player::info() const {
  std::string s = title();
  std::string v, a;
  for (auto &t : tracks()) {
    if (!t.selected) continue;
    if (t.type == "video" && v.empty()) v = t.codec;
    if (t.type == "audio" && a.empty()) a = t.codec;
  }
  std::string size;
  if (vsink_) {
    // the size of the stream before scaling: ask the converter's input
    GstElement *conv = gst_bin_get_by_name(GST_BIN(vsink_), "convert");
    if (conv) {
      GstPad *pad = gst_element_get_static_pad(conv, "sink");
      if (GstCaps *caps = pad ? gst_pad_get_current_caps(pad) : nullptr) {
        GstVideoInfo vi;
        if (gst_video_info_from_caps(&vi, caps)) {
          char b[64];
          snprintf(b, sizeof b, "%dx%d", vi.width, vi.height);
          size = b;
          if (vi.fps_n > 0 && vi.fps_d > 0) {
            snprintf(b, sizeof b, " %.3g fps", (double)vi.fps_n / vi.fps_d);
            size += b;
          }
        }
        gst_caps_unref(caps);
      }
      if (pad) gst_object_unref(pad);
      gst_object_unref(conv);
    }
  }
  std::string line;
  if (!v.empty()) line += v;
  if (!size.empty()) line += (line.empty() ? "" : " ") + size;
  if (!a.empty()) line += (line.empty() ? "" : "  ") + a;
  return line.empty() ? s : s + "\n" + line;
}

bool Player::screenshot(const std::string &dir, std::string &saved, std::string &err) {
  if (!playbin_ || cur_ < 0) { err = "Nothing is playing"; return false; }
  GstSample *sample = nullptr;
  g_object_get(playbin_, "sample", &sample, NULL);
  if (!sample) { err = "No video frame"; return false; }
  GstCaps *png = gst_caps_from_string("image/png");
  GError *e = nullptr;
  GstSample *out = gst_video_convert_sample(sample, png, 5 * GST_SECOND, &e);
  gst_caps_unref(png);
  gst_sample_unref(sample);
  if (!out) {
    err = e ? e->message : "Could not convert the frame";
    if (e) g_error_free(e);
    return false;
  }
  GstBuffer *buf = gst_sample_get_buffer(out);
  GstMapInfo map;
  bool ok = false;
  if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
    char stamp[32];
    time_t t = time(nullptr);
    strftime(stamp, sizeof stamp, "%Y-%m-%d_%H-%M-%S", localtime(&t));
    std::string base = dir + "/fl-video-" + stamp;
    saved = base + ".png";
    for (int n = 2; fld::file_exists(saved); n++) saved = base + "-" + std::to_string(n) + ".png";
    ok = fld::write_file(saved, std::string((const char *)map.data, map.size));
    if (!ok) err = "Could not write " + saved;
    gst_buffer_unmap(buf, &map);
  }
  gst_sample_unref(out);
  return ok;
}

void Player::save_position() {
  if (cur_ < 0 || !resume_) return;
  double p = position(), d = duration();
  if (p < 0) return;
  fld::Ini ini;
  ini.load(positions_path());
  video::resume_put(ini, list_[(size_t)cur_].location, p, d);
  fld::mkdir_p(fld::dirname(positions_path()));
  ini.save(positions_path());
}

// ---- bus --------------------------------------------------------------------------------

void Player::finish_item() {
  // EOS: repeat, next item, or stay at the end
  if (loop_) {
    seek(0, false);
    set_state_playing(true);
    return;
  }
  if (resume_ && cur_ >= 0) {
    fld::Ini ini;
    ini.load(positions_path());
    video::resume_put(ini, list_[(size_t)cur_].location, 0, 0);
    ini.save(positions_path());
  }
  if (cur_ + 1 < (int)list_.size()) {
    start_item(cur_ + 1, true);
    return;
  }
  eos_ = true;
  paused_ = true;
  gst_element_set_state(playbin_, GST_STATE_PAUSED);
  if (on_state) on_state();
}

void Player::handle(GstMessage *m) {
  switch (GST_MESSAGE_TYPE(m)) {
    case GST_MESSAGE_EOS:
      finish_item();
      break;
    case GST_MESSAGE_ERROR: {
      GError *e = nullptr;
      gchar *dbg = nullptr;
      gst_message_parse_error(m, &e, &dbg);
      std::string msg = e ? e->message : "playback error";
      if (getenv("FLD_VIDEO_DEBUG") && dbg) fprintf(stderr, "fl-video: %s\n%s\n", msg.c_str(), dbg);
      if (e) g_error_free(e);
      g_free(dbg);
      gst_element_set_state(playbin_, GST_STATE_READY);
      paused_ = true;
      if (on_error) on_error(msg);
      if (on_state) on_state();
      break;
    }
    case GST_MESSAGE_ELEMENT:
      if (gst_is_missing_plugin_message(m)) {
        gchar *d = gst_missing_plugin_message_get_description(m);
        std::string msg = std::string("A GStreamer plugin is missing: ") + (d ? d : "unknown");
        g_free(d);
        if (on_error) on_error(msg);
      }
      break;
    case GST_MESSAGE_TAG: {
      GstTagList *tags = nullptr;
      gst_message_parse_tag(m, &tags);
      std::string t = tag_string(tags, GST_TAG_TITLE);
      std::string artist = tag_string(tags, GST_TAG_ARTIST);
      if (tags) gst_tag_list_unref(tags);
      if (!t.empty()) {
        std::string full = artist.empty() ? t : artist + " – " + t;
        if (full != tag_title_) {
          tag_title_ = full;
          if (on_media) on_media();
        }
      }
      break;
    }
    case GST_MESSAGE_BUFFERING: {
      int pct = 100;
      gst_message_parse_buffering(m, &pct);
      bool live = false;
      // live streams do not buffer like this
      if (!live) {
        if (pct < 100 && !buffering_) {
          buffering_ = true;
          if (want_playing_) gst_element_set_state(playbin_, GST_STATE_PAUSED);
        } else if (pct >= 100 && buffering_) {
          buffering_ = false;
          if (want_playing_) gst_element_set_state(playbin_, GST_STATE_PLAYING);
        }
      }
      if (on_buffering) on_buffering(pct);
      break;
    }
    case GST_MESSAGE_ASYNC_DONE: {
      GstQuery *q = gst_query_new_seeking(GST_FORMAT_TIME);
      if (gst_element_query(playbin_, q)) {
        gboolean s = FALSE;
        gst_query_parse_seeking(q, nullptr, &s, nullptr, nullptr);
        seekable_ = s;
      }
      gst_query_unref(q);
      if (pending_seek_ > 0) {
        double p = pending_seek_;
        pending_seek_ = -1;
        double d = duration();
        if (seekable_ && (d <= 0 || p < d - 1)) seek(p, true);
      }
      if (on_position) on_position();
      break;
    }
    case GST_MESSAGE_STREAM_START:
    case GST_MESSAGE_STREAMS_SELECTED:
      if (on_tracks) on_tracks();
      if (on_media) on_media();
      break;
    case GST_MESSAGE_DURATION_CHANGED:
      if (on_position) on_position();
      break;
    default:
      break;
  }
}

void Player::poll_cb(void *d) {
  Player *p = static_cast<Player *>(d);
  GstBus *bus = gst_element_get_bus(p->playbin_);
  while (GstMessage *m = gst_bus_pop(bus)) {
    p->handle(m);
    gst_message_unref(m);
  }
  gst_object_unref(bus);
  // position updates, a few times a second
  static int tick = 0;
  if (++tick % 5 == 0 && p->cur_ >= 0) {
    double pos = p->position(), dur = p->duration();
    if (std::fabs(pos - p->last_pos_) > 0.05 || dur != p->last_dur_) {
      p->last_pos_ = pos;
      p->last_dur_ = dur;
      if (p->on_position) p->on_position();
    }
  }
  Fl::repeat_timeout(0.05, poll_cb, d);
}
