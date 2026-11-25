#include "gst_player.h"
#include "webhelpers.h"

#include <FL/Fl.H>
#include <FL/Fl_JPEG_Image.H>
#include <FL/Fl_PNG_Image.H>
#include <unistd.h>
#include <gst/audio/streamvolume.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <gst/tag/tag.h>
#include <gst/video/video.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

// GStreamer (its plugin registry, playbin and the sinks: tens of MB) starts
// with the first item played, not with the window
Player::Player() {}

bool Player::ensure_engine() {
  if (playbin_) return true;
  if (!init_error_.empty()) return false;
  if (!fgst::init(&init_error_)) {
    if (init_error_.empty()) init_error_ = "GStreamer could not be initialized";
    return false;
  }
  playbin_ = gst_element_factory_make("playbin", "player");
  if (!playbin_) {
    init_error_ = "The GStreamer \"playbin\" element is missing (install the GStreamer base plugins).";
    return false;
  }
  gst_object_ref_sink(playbin_);
  vsink_ = fgst::make_sink(&slot_);
  if (vsink_) g_object_set(playbin_, "video-sink", vsink_, NULL);
  fgst::set_scale_to(vsink_, scale_w_, scale_h_);
  // keep the pitch when playing faster or slower
  if (GstElement *st = gst_element_factory_make("scaletempo", nullptr)) g_object_set(playbin_, "audio-filter", st, NULL);
  if (getenv("FLD_VIDEO_DEBUG")) gst_debug_set_threshold_from_string("*:2", TRUE);
  set_volume(volume_);
  set_mute(muted_);
  Fl::add_timeout(0.05, poll_cb, this);
  return true;
}

void Player::set_scale_to(int w, int h) {
  scale_w_ = w;
  scale_h_ = h;
  if (vsink_) fgst::set_scale_to(vsink_, w, h);
}

Player::~Player() {
  Fl::remove_timeout(poll_cb, this);
  if (playbin_) {
    gst_element_set_state(playbin_, GST_STATE_NULL);
    gst_object_unref(playbin_);
  }
}

// ---- playlist ---------------------------------------------------------------------------

void Player::append(const Item &it) {
  list_.push_back(it);
  int i = (int)list_.size() - 1;
  // shuffled: the new item goes somewhere after the current one
  if (shuffle_ && !order_.empty()) {
    auto pos = std::find(order_.begin(), order_.end(), cur_);
    size_t after = pos == order_.end() ? 0 : (size_t)(pos - order_.begin()) + 1;
    size_t at = after + (size_t)rand() % (order_.size() - after + 1);
    order_.insert(order_.begin() + (long)at, i);
  } else {
    order_.push_back(i);
  }
}

void Player::load(const std::string &loc, bool append_it) {
  std::vector<Item> items;
  if (media::is_playlist(loc) && !video::is_url(loc)) {
    std::string text;
    if (fld::read_file(loc, text, 8u << 20)) {
      bool pls = fld::lower(fld::extension(loc)) == "pls";
      for (const media::Entry &e : media::parse_playlist(text, fld::dirname(loc), pls)) {
        Item it;
        it.location = e.location;
        it.title = e.title;
        it.length = e.length;
        items.push_back(it);
      }
    }
  } else {
    Item it;
    it.location = loc;
    items.push_back(it);
  }
  if (items.empty()) return;
  if (!append_it) {
    save_position();
    list_.clear();
    order_.clear();
    cur_ = -1;
  }
  int first = (int)list_.size();
  for (const Item &it : items) append(it);
  if (shuffle_ && !append_it) rebuild_order();
  if (on_playlist) on_playlist();
  if (!append_it || cur_ < 0) start_item(shuffle_ && !append_it ? order_.front() : first, true);
  else if (on_media) on_media();
}

void Player::play_index(int i) {
  if (i < 0 || i >= (int)list_.size()) return;
  save_position();
  start_item(i, true);
}

bool Player::has_next() const {
  return cur_ >= 0 && (repeat_ == media::Repeat::All ? list_.size() > 1 : media::step(order_, cur_, 1, media::Repeat::Off) >= 0);
}

bool Player::has_prev() const { return cur_ >= 0; }

void Player::next() {
  // Next always moves on, even when the item repeats
  media::Repeat r = repeat_ == media::Repeat::One ? media::Repeat::Off : repeat_;
  int n = media::step(order_, cur_, 1, r);
  if (n >= 0) play_index(n);
}

void Player::prev() {
  // like most players: back to the start first, then the previous item
  media::Repeat r = repeat_ == media::Repeat::One ? media::Repeat::Off : repeat_;
  int p = media::step(order_, cur_, -1, r);
  if (position() > 3 || p < 0) seek(0, false);
  else play_index(p);
}

void Player::rebuild_order() {
  order_.clear();
  if (shuffle_) {
    order_ = media::shuffle_order((int)list_.size(), cur_,
                                  (unsigned)std::chrono::steady_clock::now().time_since_epoch().count());
  } else {
    for (int i = 0; i < (int)list_.size(); i++) order_.push_back(i);
  }
}

void Player::set_shuffle(bool s) {
  shuffle_ = s;
  rebuild_order();
}

void Player::remove(int i) {
  if (i < 0 || i >= (int)list_.size()) return;
  bool was_current = i == cur_;
  list_.erase(list_.begin() + i);
  // the order loses i; later indices move down
  order_.erase(std::remove(order_.begin(), order_.end(), i), order_.end());
  for (int &o : order_)
    if (o > i) o--;
  if (cur_ > i) cur_--;
  if (was_current) {
    if (list_.empty()) {
      clear();
      return;
    }
    // the item that took its place plays
    start_item(std::min(i, (int)list_.size() - 1), !paused_);
  }
  if (on_playlist) on_playlist();
  if (on_media) on_media();
}

void Player::move(int from, int to) {
  int n = (int)list_.size();
  if (from < 0 || from >= n || to < 0 || to >= n || from == to) return;
  Item it = list_[(size_t)from];
  list_.erase(list_.begin() + from);
  list_.insert(list_.begin() + to, it);
  auto remap = [from, to](int k) {
    if (k == from) return to;
    if (from < to && k > from && k <= to) return k - 1;
    if (from > to && k >= to && k < from) return k + 1;
    return k;
  };
  cur_ = cur_ >= 0 ? remap(cur_) : cur_;
  if (shuffle_) {
    for (int &o : order_) o = remap(o);
  } else {
    rebuild_order();
  }
  if (on_playlist) on_playlist();
  if (on_media) on_media();
}

void Player::clear() {
  save_position();
  if (playbin_) gst_element_set_state(playbin_, GST_STATE_READY);
  list_.clear();
  order_.clear();
  cur_ = -1;
  paused_ = true;
  eos_ = false;
  tag_title_.clear();
  tag_artist_.clear();
  tag_album_.clear();
  tag_track_.clear();
  cover_path_.clear();
  slot_.clear();
  if (on_playlist) on_playlist();
  if (on_media) on_media();
  if (on_state) on_state();
}

bool Player::save_playlist(const std::string &path, std::string &err) const {
  std::vector<media::Entry> entries;
  for (const Item &it : list_) {
    media::Entry e;
    e.location = it.location;
    e.title = it.title;
    e.length = it.length;
    entries.push_back(e);
  }
  bool pls = fld::lower(fld::extension(path)) == "pls";
  if (!fld::write_file(path, pls ? media::write_pls(entries) : media::write_m3u(entries))) {
    err = "Could not write " + path;
    return false;
  }
  return true;
}

// ---- cover art ------------------------------------------------------------------------

void Player::set_cover(const std::string &bytes, const std::string &mime, bool from_tags) {
  Fl_RGB_Image *img = nullptr;
  bool png = mime.find("png") != std::string::npos || bytes.compare(0, 4, "\x89PNG") == 0;
  if (png) img = new Fl_PNG_Image(nullptr, (const unsigned char *)bytes.data(), (int)bytes.size());
  else img = new Fl_JPEG_Image(nullptr, (const unsigned char *)bytes.data());
  if (!img || img->fail() || img->w() <= 0 || img->h() <= 0 || img->d() < 1) {
    delete img;
    return;
  }
  auto f = std::make_shared<fgst::Frame>();
  f->w = img->w();
  f->h = img->h();
  f->rgba.resize((size_t)f->w * f->h * 4);
  const unsigned char *src = (const unsigned char *)img->data()[0];
  int d = img->d(), ld = img->ld() ? img->ld() : img->w() * d;
  for (int y = 0; y < f->h; y++)
    for (int x = 0; x < f->w; x++) {
      const unsigned char *p = src + (size_t)y * ld + (size_t)x * d;
      uint8_t *o = &f->rgba[((size_t)y * f->w + x) * 4];
      o[0] = d >= 3 ? p[0] : p[0];
      o[1] = d >= 3 ? p[1] : p[0];
      o[2] = d >= 3 ? p[2] : p[0];
      o[3] = 255;
    }
  delete img;
  slot_.set(f);
  cover_from_tags_ = from_tags;
  // a file for MPRIS (mpris:artUrl)
  const char *rt = getenv("XDG_RUNTIME_DIR");
  std::string path = std::string(rt && *rt ? rt : "/tmp") + "/fl-media-player-" + std::to_string(getpid()) +
                     (png ? "-cover.png" : "-cover.jpg");
  if (fld::write_file(path, bytes)) cover_path_ = path;
  if (on_tags) on_tags();
}

void Player::find_folder_cover() {
  if (cur_ < 0) return;
  const std::string &loc = list_[(size_t)cur_].location;
  if (video::is_url(loc)) return;
  std::string dir = fld::dirname(loc);
  for (const char *n : {"cover.jpg", "cover.png", "folder.jpg", "folder.png", "front.jpg", "Cover.jpg", "Folder.jpg",
                        "album.jpg", "AlbumArt.jpg"}) {
    std::string data;
    if (fld::read_file(dir + "/" + n, data, 16u << 20) && !data.empty()) {
      set_cover(data, fld::ends_with(n, ".png") ? "image/png" : "image/jpeg", false);
      return;
    }
  }
}

void Player::start_item(int i, bool play) {
  if (!ensure_engine()) {
    if (on_error) on_error("Could not start the GStreamer playback engine: " + init_error_);
    return;
  }
  const Item &item = list_[(size_t)i];
  if (item.resolved.empty() && video::needs_resolver(item.location) &&
      (!webhelpers::ytdlp_path().empty() || webhelpers::updating())) {
    resolve_and_start(i, play);
    return;
  }
  cur_ = i;
  eos_ = false;
  seekable_ = false;
  tag_title_.clear();
  tag_artist_.clear();
  tag_album_.clear();
  tag_track_.clear();
  cover_path_.clear();
  cover_from_tags_ = false;
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
  if (on_tags) on_tags();
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
  std::vector<std::string> cmd = webhelpers::resolve_command(loc);
  bool downloading = webhelpers::updating() && webhelpers::ytdlp_path().empty();
  std::thread([job, cmd, downloading] {
    if (downloading) { // the first start: our yt-dlp is still on its way
      for (int i = 0; i < 600 && webhelpers::updating(); i++) usleep(100000);
    }
    std::vector<std::string> c = cmd;
    if (downloading) c = webhelpers::resolve_command(job->loc);
    std::string out;
    // stderr too: yt-dlp says there what went wrong
    int rc = fld::run_capture_input(c, "", out, true);
    std::vector<std::string> lines, errors;
    for (auto &l : fld::split(fld::trim(out), '\n')) {
      std::string t = fld::trim(l);
      if (t.empty() || fld::starts_with(t, "WARNING:") || fld::starts_with(t, "[")) continue;
      if (fld::starts_with(t, "ERROR:")) errors.push_back(fld::trim(t.substr(6)));
      else lines.push_back(t);
    }
    if (rc == 0 && lines.size() >= 2) {
      job->title = lines[0];
      job->url = lines.back();
    } else if (rc == 127 || rc < 0) {
      job->err = "yt-dlp is not available. It is downloaded when the player starts (Python 3 is needed), "
                 "or install it yourself.";
    } else {
      job->err = !errors.empty() ? errors.back() : !lines.empty() ? lines.back() : "yt-dlp could not find a stream";
      // usually: the site changed and yt-dlp did not keep up
      if (webhelpers::load_state().manage) job->err += " (yt-dlp may be out of date: checking for a new one)";
    }
    Fl::awake([](void *d) {
      Job *j = static_cast<Job *>(d);
      Player *p = j->p;
      if (j->index < (int)p->list_.size() && p->list_[(size_t)j->index].location == j->loc) {
        if (!j->url.empty()) {
          p->list_[(size_t)j->index].resolved = j->url;
          if (!j->title.empty()) p->list_[(size_t)j->index].title = j->title;
          p->start_item(j->index, j->play);
        } else {
          if (p->on_error) p->on_error(j->err);
          if (webhelpers::load_state().manage && !webhelpers::updating())
            webhelpers::update([](bool, bool, const std::string &) {});
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
  if (!playbin_) return;
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
  if (playbin_) gst_element_set_state(playbin_, GST_STATE_NULL);
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
  if (!playbin_) return;
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
  if (!playbin_) return;
  if (type == "sub") set_subtitles_visible(true);
  g_object_set(playbin_, prop, index, NULL);
  if (on_tracks) on_tracks();
}

void Player::cycle_track(const std::string &type) {
  const char *n_prop = type == "video" ? "n-video" : type == "audio" ? "n-audio" : "n-text";
  const char *cur_prop = type == "video" ? "current-video" : type == "audio" ? "current-audio" : "current-text";
  if (!playbin_) return;
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
  if (!playbin_) return;
  int flags = 0;
  g_object_get(playbin_, "flags", &flags, NULL);
  flags = v ? (flags | PLAY_FLAG_TEXT) : (flags & ~PLAY_FLAG_TEXT);
  g_object_set(playbin_, "flags", flags, NULL);
  if (on_tracks) on_tracks();
}

bool Player::subtitles_visible() const {
  if (!playbin_) return false;
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
    std::string base = dir + "/fl-media-player-" + stamp;
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
  if (repeat_ == media::Repeat::One) {
    seek(0, false);
    set_state_playing(true);
    return;
  }
  if (cur_ >= 0) {
    double d = duration();
    if (d > 0) list_[(size_t)cur_].length = d;
  }
  if (resume_ && cur_ >= 0) {
    fld::Ini ini;
    ini.load(positions_path());
    video::resume_put(ini, list_[(size_t)cur_].location, 0, 0);
    ini.save(positions_path());
  }
  int n = media::step(order_, cur_, 1, repeat_);
  if (n >= 0) {
    start_item(n, true);
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
      if (getenv("FLD_VIDEO_DEBUG") && dbg) fprintf(stderr, "fl-media-player: %s\n%s\n", msg.c_str(), dbg);
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
      std::string album = tag_string(tags, GST_TAG_ALBUM);
      bool changed = false;
      if (!artist.empty() && artist != tag_artist_) tag_artist_ = artist, changed = true;
      if (!album.empty() && album != tag_album_) tag_album_ = album, changed = true;
      if (!t.empty() && t != tag_track_) tag_track_ = t, changed = true;
      // the embedded cover (front cover first), once per item
      if (!cover_from_tags_ && tags) {
        GstSample *img = nullptr;
        if (!gst_tag_list_get_sample(tags, GST_TAG_IMAGE, &img)) gst_tag_list_get_sample(tags, GST_TAG_PREVIEW_IMAGE, &img);
        if (img) {
          GstBuffer *b = gst_sample_get_buffer(img);
          GstCaps *caps = gst_sample_get_caps(img);
          std::string mime;
          if (caps && gst_caps_get_size(caps) > 0) mime = gst_structure_get_name(gst_caps_get_structure(caps, 0));
          GstMapInfo map;
          if (b && gst_buffer_map(b, &map, GST_MAP_READ)) {
            set_cover(std::string((const char *)map.data, map.size), mime, true);
            gst_buffer_unmap(b, &map);
          }
          gst_sample_unref(img);
        }
      }
      if (tags) gst_tag_list_unref(tags);
      if (changed && on_tags) on_tags();
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
      // music without an embedded cover: cover.jpg and the like in its folder
      if (cover_path_.empty() && !has_video()) find_folder_cover();
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
