// fl-media-player: video and music player for fldesktop, built on GStreamer
// (fl-video before 0.1.4)
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Return_Button.H>
#include <functional>
#include <memory>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Slider.H>
#include <FL/Fl_Choice.H>
#include <FL/Fl_Button.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>

#include <cmath>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "idle_inhibit.h"
#include "gst_player.h"
#include "webhelpers.h"
#include "playlist_util.h"
#include "video_util.h"
#ifdef FLD_HAVE_MPRIS
#include "mpris_server.h"
#endif

static const int BAR = 46;
static const int SIDE = 300; // the playlist sidebar

struct Settings {
  double volume = 100;
  bool resume = true;
  bool playlist = false; // the sidebar shows
  bool shuffle = false;
  int repeat = 0;        // media::Repeat
  bool replaygain = true; // volume levelling from ReplayGain tags
  std::vector<double> eq = std::vector<double>(10, 0.0); // the equalizer, dB
} S;

static std::string settings_path() { return fld::fld_config_dir() + "/media-player.ini"; }

static void load_settings() {
  fld::Ini c;
  // fl-video kept its settings in video.ini
  if (!c.load(settings_path())) c.load(fld::fld_config_dir() + "/video.ini");
  S.volume = std::max(0.0, std::min(130.0, c.get_double("Video", "Volume", 100)));
  S.resume = c.get_bool("Video", "ResumePlayback", true);
  S.playlist = c.get_bool("Player", "ShowPlaylist", false);
  S.shuffle = c.get_bool("Player", "Shuffle", false);
  S.repeat = std::max(0, std::min(2, c.get_int("Player", "Repeat", 0)));
  S.replaygain = c.get_bool("Player", "ReplayGain", true);
  auto eq = fld::split(c.get("Player", "Equalizer", ""), ',');
  for (size_t i = 0; i < eq.size() && i < 10; i++) S.eq[i] = atof(eq[i].c_str());
}

static void save_settings() {
  fld::Ini c;
  c.load(settings_path());
  c.set_int("Video", "Volume", (int)std::lround(S.volume));
  c.set_bool("Video", "ResumePlayback", S.resume);
  c.set_bool("Player", "ShowPlaylist", S.playlist);
  c.set_bool("Player", "Shuffle", S.shuffle);
  c.set_int("Player", "Repeat", S.repeat);
  c.set_bool("Player", "ReplayGain", S.replaygain);
  std::string eq;
  for (size_t i = 0; i < S.eq.size(); i++) {
    char b[16];
    snprintf(b, sizeof b, "%s%.1f", i ? "," : "", S.eq[i]);
    eq += b;
  }
  c.set("Player", "Equalizer", eq);
  c.save(settings_path());
}

// ---------------------------------------------------------------------------

class VideoWindow;
static VideoWindow *g_win;
static Player *g_player;
static fgst::FrameView *g_surface;
static Fl_Group *g_bar;
static Fl_Slider *g_seek, *g_vol;
static Fl_Box *g_time;
static fld::FlatButton *g_play, *g_prev, *g_next, *g_mute, *g_full;
static bool g_fullscreen = false, g_bar_visible = true, g_force_sw = false;
// OpenGL (Mesa: tens of MB) waits for the first video; music and an empty
// window need only the software surface
static bool g_gl_deferred = false;
static bool g_seeking = false;
// the playlist sidebar
static Fl_Group *g_side;
static Fl_Hold_Browser *g_list;
static fld::FlatButton *g_shuffle_btn, *g_repeat_btn, *g_list_btn;
static void fill_playlist();
static void update_mpris();

static void layout();
static void update_play_button();
static void show_menu(int x, int y);
static void toggle_fullscreen();

static void set_icon(fld::FlatButton *b, const char *icon, const char *fallback) {
  if (fld::icon(icon, 20)) {
    b->icon_name(icon, 20);
    b->label(nullptr);
  } else {
    b->icon_image(nullptr);
    b->label(fallback);
  }
}

// ---- open ---------------------------------------------------------------------------

static void open_files(const std::vector<std::string> &files, bool append) {
  bool first = true;
  for (auto &f : files) {
    if (video::is_subtitle(f) && g_player->has_media()) {
      g_player->add_subtitle(f);
      continue;
    }
    g_player->load(f, append || !first);
    first = false;
  }
}

static const char *kMediaFilter =
    "Media Files (*.{mp4,m4v,mkv,webm,avi,mov,wmv,flv,ogv,mpg,mpeg,ts,m2ts,3gp,mp3,ogg,oga,opus,flac,wav,m4a,aac,"
    "wma,mka,m3u,m3u8,pls})\tVideo (*.{mp4,m4v,mkv,webm,avi,mov,wmv,flv,ogv,mpg,mpeg,ts,m2ts,3gp})\t"
    "Audio (*.{mp3,ogg,oga,opus,flac,wav,m4a,aac,wma,mka})\tAll Files (*)";

static std::string g_last_dir;

static void open_dialog(bool append) {
  std::string dir = g_last_dir.empty() ? fld::user_dir("VIDEOS") : g_last_dir;
  if (g_last_dir.empty() && S.playlist && fld::is_dir(fld::user_dir("MUSIC"))) dir = fld::user_dir("MUSIC");
  if (dir.empty() || !fld::is_dir(dir)) dir = fld::home_dir();
  Fl_File_Chooser fc(dir.c_str(), kMediaFilter, Fl_File_Chooser::MULTI, append ? "Add to Playlist" : "Open Media");
  fc.show();
  while (fc.shown()) Fl::wait();
  std::vector<std::string> files;
  for (int i = 1; i <= fc.count(); i++)
    if (fc.value(i)) files.push_back(fc.value(i));
  if (!files.empty()) g_last_dir = fld::dirname(files[0]);
  open_files(files, append);
}

static void open_url() {
  const char *u = fl_input("Open a network stream or web address:", "");
  if (u && *u) open_files({fld::trim(u)}, false);
}

static void add_subtitles() {
  std::string dir = g_last_dir.empty() ? fld::home_dir() : g_last_dir;
  const char *f = fl_file_chooser("Load Subtitles", "Subtitles (*.{srt,ass,ssa,vtt,sub})\tAll Files (*)",
                                  dir.c_str(), 0);
  if (f) g_player->add_subtitle(f);
}

// ---- controls ---------------------------------------------------------------------------

static void update_time() {
  double pos = g_player->has_media() ? g_player->position() : -1, dur = g_player->has_media() ? g_player->duration() : -1;
  std::string t = video::format_time(pos) + " / " + video::format_time(dur);
  g_time->copy_label(t.c_str());
  if (!g_seeking) {
    g_seek->range(0, dur > 0 ? dur : 1);
    g_seek->value(pos > 0 ? pos : 0);
    g_seek->redraw();
  }
  if (dur > 0 && g_player->seekable()) g_seek->activate();
  else g_seek->deactivate();
}

static void update_volume() {
  bool muted = g_player->muted();
  double v = g_player->volume();
  set_icon(g_mute, muted || v <= 0 ? "audio-volume-muted" : v < 40 ? "audio-volume-low" : v < 80 ? "audio-volume-medium" : "audio-volume-high",
           muted ? "Muted" : "Vol");
  if (Fl::pushed() != g_vol) g_vol->value(v);
  g_mute->tooltip(muted ? "Unmute (M)" : "Mute (M)");
}

static void update_play_button() {
  bool paused = g_player->paused() || g_player->at_end();
  set_icon(g_play, paused ? "media-playback-start" : "media-playback-pause", paused ? "▶" : "⏸");
  g_play->tooltip(paused ? "Play (Space)" : "Pause (Space)");
  bool playing = !paused && g_player->has_media();
  idle::inhibit(g_win ? (Fl_Window *)g_win : nullptr, playing && g_player->has_video());
#ifdef FLD_HAVE_MPRIS
  // music: no automatic suspend while it plays (the screen may blank)
  mprisd::inhibit_idle(playing && !g_player->has_video());
#endif
  // audio only: a placeholder with the title instead of a black picture;
  // nothing opened yet: how to open something
  std::string ph = g_player->has_media() && !g_player->has_video() ? g_player->title() : "";
  const char *icon = "audio-x-generic";
  if (!g_player->has_media()) {
    ph = "Open a file or drop one here\n(click, or Ctrl+O)";
    icon = "video-x-generic";
  }
  if (ph != g_surface->osd().placeholder || icon != g_surface->osd().placeholder_icon) {
    g_surface->osd().placeholder = ph;
    g_surface->osd().placeholder_icon = icon;
    g_surface->redraw();
  }
}

static void update_title() {
  std::string t = g_player->title();
  long n = g_player->count(), i = g_player->current();
  if (n > 1 && i >= 0) t += " (" + std::to_string(i + 1) + "/" + std::to_string(n) + ")";
  t = t.empty() ? "Media Player" : t + " – Media Player";
  extern void set_window_title(const std::string &);
  set_window_title(t);
  if (n > 1) { g_prev->activate(); g_next->activate(); }
  else { g_prev->deactivate(); g_next->deactivate(); }
  fill_playlist();
  update_mpris();
}

static void toggle_pause() {
  if (!g_player->has_media()) {
    open_dialog(false);
    return;
  }
  g_player->toggle_pause();
}

static void change_volume(double d) {
  S.volume = std::max(0.0, std::min(130.0, g_player->volume() + d));
  g_player->set_volume(S.volume);
  g_player->set_mute(false);
  update_volume();
  g_surface->show_message("Volume: " + std::to_string((int)std::lround(S.volume)) + "%");
}

static void seek(double secs) {
  g_player->seek_relative(secs);
}

static void set_speed(double s) {
  g_player->set_rate(s);
  char b[48];
  snprintf(b, sizeof b, "Speed: %g×", s);
  g_surface->show_message(b);
}

// ---- window ---------------------------------------------------------------------------

static void hide_bar_cb(void *) {
  if (!g_fullscreen || Fl::belowmouse() == g_bar || (Fl::belowmouse() && Fl::belowmouse()->parent() == g_bar)) {
    if (g_fullscreen) Fl::repeat_timeout(2.5, hide_bar_cb);
    return;
  }
  g_bar_visible = false;
  layout();
}

static void poke_bar() {
  if (!g_fullscreen) return;
  if (!g_bar_visible) {
    g_bar_visible = true;
    layout();
  }
  Fl::remove_timeout(hide_bar_cb);
  Fl::add_timeout(2.5, hide_bar_cb);
}

class VideoWindow : public Fl_Double_Window {
public:
  using Fl_Double_Window::Fl_Double_Window;
  void resize(int X, int Y, int W, int H) override {
    Fl_Double_Window::resize(X, Y, W, H);
    layout();
  }
  int handle(int e) override {
    switch (e) {
      case FL_DND_ENTER: case FL_DND_DRAG: case FL_DND_RELEASE: return 1;
      case FL_PASTE: {
        char cwd[4096];
        auto files = video::parse_uri_list(std::string(Fl::event_text(), Fl::event_length()),
                                           getcwd(cwd, sizeof cwd) ? cwd : "/");
        // dropping while something plays adds to the playlist
        open_files(files, g_player->has_media() && !(Fl::event_state() & FL_CTRL));
        return 1;
      }
      case FL_MOVE: case FL_ENTER:
        poke_bar();
        break;
    }
    return Fl_Double_Window::handle(e);
  }
};

void set_window_title(const std::string &t) { g_win->copy_label(t.c_str()); }

static void layout() {
  int W = g_win->w(), H = g_win->h();
  bool bar = !g_fullscreen || g_bar_visible;
  int vh = bar ? H - BAR : H;
  bool side = S.playlist && !g_fullscreen && W >= SIDE + 240;
  int sw = side ? SIDE : 0;
  g_surface->widget()->resize(0, 0, std::max(1, W - sw), std::max(1, vh));
  g_side->resize(W - sw, 0, SIDE, std::max(1, vh));
  if (side) {
    g_side->show();
    // the list above a row of buttons
    int sx = W - sw, sy = 0, sh = std::max(1, vh);
    g_list->resize(sx, sy, SIDE, std::max(1, sh - 40));
    int bx = sx + 4, by = sy + sh - 38;
    for (int i = 1; i < g_side->children(); i++) {
      g_side->child(i)->resize(bx, by, 34, 34);
      bx += 36;
    }
  } else {
    g_side->hide();
  }
  g_bar->resize(0, H - BAR, W, BAR);
  if (bar) g_bar->show();
  else g_bar->hide();
  // the bar's children are laid out by hand
  int x = 8, y = H - BAR + 5;
  g_play->resize(x, y, 36, 36); x += 38;
  g_prev->resize(x, y, 36, 36); x += 38;
  g_next->resize(x, y, 36, 36); x += 44;
  int right = W - 8;
  fld::FlatButton *menu = (fld::FlatButton *)g_bar->child(g_bar->children() - 1);
  menu->resize(right - 36, y, 36, 36); right -= 38;
  g_full->resize(right - 36, y, 36, 36); right -= 38;
  g_list_btn->resize(right - 36, y, 36, 36); right -= 44;
  g_vol->resize(right - 90, y + 10, 90, 16); right -= 94;
  g_mute->resize(right - 36, y, 36, 36); right -= 46;
  g_time->resize(right - 130, y, 130, 36); right -= 138;
  g_seek->resize(x, y + 9, std::max(40, right - x), 18);
  g_win->redraw();
}

static void toggle_fullscreen() {
  g_fullscreen = !g_fullscreen;
  if (g_fullscreen) {
    g_win->fullscreen();
    g_bar_visible = false;
    Fl::remove_timeout(hide_bar_cb);
  } else {
    g_win->fullscreen_off();
    g_bar_visible = true;
  }
  set_icon(g_full, g_fullscreen ? "view-restore" : "view-fullscreen", g_fullscreen ? "⤡" : "⛶");
  layout();
}

static void quit() {
  g_player->save_position();
  S.volume = g_player->volume();
  save_settings();
  idle::inhibit(g_win, false);
#ifdef FLD_HAVE_MPRIS
  mprisd::inhibit_idle(false);
#endif
  g_win->hide();
}

// ---- playlist sidebar ---------------------------------------------------------------------

static std::vector<std::string> g_list_labels;

static std::string item_name(const Player::Item &it) {
  if (!it.title.empty()) return it.title;
  if (video::is_url(it.location)) return it.location;
  return fld::basename(it.location);
}

static void fill_playlist() {
  if (!g_list) return;
  const auto &pl = g_player->playlist();
  int top = g_list->topline();
  g_list->clear();
  for (size_t i = 0; i < pl.size(); i++) {
    std::string name = (int)i == g_player->current() && !g_player->track_title().empty()
                           ? (g_player->artist().empty() ? g_player->track_title()
                                                         : g_player->artist() + " – " + g_player->track_title())
                           : item_name(pl[i]);
    std::string len = pl[i].length > 0 ? video::format_time(pl[i].length) : "";
    std::string line = std::string((int)i == g_player->current() ? "@b" : "") + std::to_string(i + 1) + ". " + name +
                       "\t@r" + len;
    g_list->add(line.c_str());
  }
  if (pl.empty()) g_list->add("@iDrop files here or press + to add");
  g_list->topline(std::max(1, std::min(top, g_list->size())));
  if (g_player->current() >= 0) g_list->select(g_player->current() + 1);
}

static void update_order_buttons() {
  g_shuffle_btn->value(g_player->shuffle());
  g_shuffle_btn->tooltip(g_player->shuffle() ? "Shuffle: on" : "Shuffle: off");
  static const char *rep[] = {"Repeat: off", "Repeat: all", "Repeat: one"};
  media::Repeat r = g_player->repeat();
  g_repeat_btn->value(r != media::Repeat::Off);
  g_repeat_btn->tooltip(rep[(int)r]);
  set_icon(g_repeat_btn, "media-playlist-repeat",
           r == media::Repeat::One ? "1" : "Rep");
  g_shuffle_btn->redraw();
  g_repeat_btn->redraw();
}

static void set_shuffle(bool on) {
  g_player->set_shuffle(on);
  S.shuffle = on;
  save_settings();
  update_order_buttons();
  update_mpris();
}

static void set_repeat(media::Repeat r) {
  g_player->set_repeat(r);
  S.repeat = (int)r;
  save_settings();
  update_order_buttons();
  update_mpris();
}

static void toggle_playlist() {
  S.playlist = !S.playlist;
  save_settings();
  g_list_btn->value(S.playlist);
  layout();
}

static void save_playlist() {
  std::string dir = fld::user_dir("MUSIC");
  if (dir.empty() || !fld::is_dir(dir)) dir = fld::home_dir();
  Fl_File_Chooser fc((dir + "/Playlist.m3u").c_str(), "M3U playlist (*.{m3u,m3u8})\tPLS playlist (*.pls)",
                     Fl_File_Chooser::CREATE, "Save Playlist");
  fc.show();
  while (fc.shown()) Fl::wait();
  if (!fc.value()) return;
  std::string path = fc.value();
  if (!media::is_playlist(path)) path += ".m3u";
  std::string err;
  if (g_player->save_playlist(path, err)) g_surface->show_message("Saved " + fld::basename(path));
  else fl_alert("%s", err.c_str());
}

static void playlist_menu(int mx, int my) {
  int sel = g_list->value() - 1;
  bool have = sel >= 0 && sel < g_player->count();
  Fl_Menu_Item m[] = {
      {"Play", 0, nullptr, (void *)1, have ? 0 : FL_MENU_INACTIVE},
      {"Remove", FL_Delete, nullptr, (void *)2, have ? 0 : FL_MENU_INACTIVE},
      {"Move Up", 0, nullptr, (void *)3, have && sel > 0 ? 0 : FL_MENU_INACTIVE},
      {"Move Down", 0, nullptr, (void *)4, (have && sel + 1 < g_player->count() ? 0 : FL_MENU_INACTIVE) | FL_MENU_DIVIDER},
      {"Add Files…", 0, nullptr, (void *)5},
      {"Open Playlist…", 0, nullptr, (void *)6},
      {"Save Playlist…", 0, nullptr, (void *)7, g_player->count() ? 0 : FL_MENU_INACTIVE},
      {"Clear", 0, nullptr, (void *)8, g_player->count() ? 0 : FL_MENU_INACTIVE},
      {nullptr}};
  const Fl_Menu_Item *r = m->popup(mx, my);
  if (!r) return;
  switch ((long)r->user_data()) {
    case 1: g_player->play_index(sel); break;
    case 2: g_player->remove(sel); break;
    case 3: g_player->move(sel, sel - 1); g_list->select(sel); break;
    case 4: g_player->move(sel, sel + 1); g_list->select(sel + 2); break;
    case 5: open_dialog(true); break;
    case 6: open_dialog(false); break;
    case 7: save_playlist(); break;
    case 8: g_player->clear(); break;
  }
}

class PlaylistBrowser : public Fl_Hold_Browser {
public:
  using Fl_Hold_Browser::Fl_Hold_Browser;
  int handle(int e) override {
    if (e == FL_PUSH && Fl::event_button() == FL_RIGHT_MOUSE) {
      int line = find_index(Fl::event_y());
      if (line > 0) select(line);
      playlist_menu(Fl::event_x(), Fl::event_y());
      return 1;
    }
    if (e == FL_KEYBOARD && Fl::event_key() == FL_Delete && value() > 0) {
      g_player->remove(value() - 1);
      return 1;
    }
    if (e == FL_KEYBOARD && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) && value() > 0) {
      g_player->play_index(value() - 1);
      return 1;
    }
    // drop files onto the list: add them
    if (e == FL_DND_ENTER || e == FL_DND_DRAG || e == FL_DND_RELEASE) return 1;
    if (e == FL_PASTE) {
      char cwd[4096];
      auto files = video::parse_uri_list(std::string(Fl::event_text(), Fl::event_length()),
                                         getcwd(cwd, sizeof cwd) ? cwd : "/");
      open_files(files, true);
      return 1;
    }
    return Fl_Hold_Browser::handle(e);
  }
  int find_index(int y) {
    void *it = find_item(y);
    return it ? lineno(it) : 0;
  }
};

// ---- MPRIS ------------------------------------------------------------------------------

static void update_mpris() {
#ifdef FLD_HAVE_MPRIS
  mprisd::Status s;
  if (g_player->has_media()) {
    s.playback = g_player->at_end() ? "Stopped" : g_player->paused() ? "Paused" : "Playing";
    s.track_id = "/org/fldesktop/MediaPlayer/track/" + std::to_string(g_player->current());
    s.title = g_player->track_title().empty() ? g_player->title() : g_player->track_title();
    s.artist = g_player->artist();
    s.album = g_player->album();
    if (!g_player->cover_path().empty()) s.art_url = "file://" + g_player->cover_path();
    const auto &it = g_player->playlist()[(size_t)g_player->current()];
    s.url = video::is_url(it.location) ? it.location : video::to_uri(it.location);
    s.length = g_player->duration();
    s.position = std::max(0.0, g_player->position());
    s.can_seek = g_player->seekable();
  }
  s.volume = g_player->muted() ? 0 : g_player->volume() / 100.0;
  s.rate = g_player->rate();
  s.can_next = g_player->has_next();
  s.can_prev = g_player->has_prev();
  s.shuffle = g_player->shuffle();
  s.loop = g_player->repeat() == media::Repeat::One ? "Track" : g_player->repeat() == media::Repeat::All ? "Playlist" : "None";
  mprisd::update(s);
#endif
}

static void start_mpris() {
#ifdef FLD_HAVE_MPRIS
  mprisd::Handlers h;
  h.command = [](const std::string &m) {
    if (m == "PlayPause") toggle_pause();
    else if (m == "Play") { if (g_player->paused() || g_player->at_end()) toggle_pause(); }
    else if (m == "Pause") { if (!g_player->paused()) g_player->pause(); }
    else if (m == "Stop") { g_player->pause(); g_player->seek(0, false); }
    else if (m == "Next") g_player->next();
    else if (m == "Previous") g_player->prev();
  };
  h.seek = [](double d) {
    g_player->seek_relative(d);
    mprisd::seeked(g_player->position());
  };
  h.set_position = [](double p) {
    g_player->seek(p, true);
    mprisd::seeked(p);
  };
  h.open = [](const std::string &uri) {
    char cwd[4096];
    open_files(video::parse_uri_list(uri, getcwd(cwd, sizeof cwd) ? cwd : "/"), false);
  };
  h.set_volume = [](double v) {
    S.volume = v * 100;
    g_player->set_volume(S.volume);
    g_player->set_mute(false);
    update_volume();
    update_mpris();
  };
  h.set_shuffle = [](bool b) { set_shuffle(b); };
  h.set_loop = [](const std::string &l) {
    set_repeat(l == "Track" ? media::Repeat::One : l == "Playlist" ? media::Repeat::All : media::Repeat::Off);
  };
  h.raise = [] { g_win->show(); };
  h.quit = [] { quit(); };
  mprisd::start(h);
#endif
}

// ---- menu -------------------------------------------------------------------------------

static std::vector<std::string> g_labels;
static std::vector<Player::Track> g_tracks;

static std::string esc(const std::string &t) {
  std::string e;
  for (char c : t) {
    if (c == '/' || c == '\\' || c == '&' || c == '_' || c == '@') e += '\\';
    e += c;
  }
  return e;
}

static std::string track_label(const Player::Track &t) {
  std::string l = "Track " + std::to_string(t.index + 1);
  if (!t.title.empty()) l += ": " + t.title;
  if (!t.lang.empty()) l += " [" + t.lang + "]";
  if (!t.codec.empty() && t.title.empty()) l += " (" + t.codec + ")";
  return l;
}

static void take_screenshot() {
  std::string dir = fld::user_dir("PICTURES");
  if (dir.empty() || !fld::is_dir(dir)) dir = fld::home_dir();
  std::string saved, err;
  if (g_player->screenshot(dir, saved, err)) g_surface->show_message("Screenshot: " + fld::basename(saved), 3);
  else g_surface->show_message("No screenshot: " + err, 3);
}

static void toggle_loop() {
  // off, all, one
  set_repeat(media::next_repeat(g_player->repeat()));
  static const char *names[] = {"Repeat: off", "Repeat: the playlist", "Repeat: this item"};
  g_surface->show_message(names[(int)g_player->repeat()]);
}

static void toggle_mute() {
  g_player->set_mute(!g_player->muted());
  update_volume();
}

enum { M_OPEN = 1, M_ADD, M_URL, M_SUBS, M_SHOT, M_FULL, M_RESUME, M_LOOP, M_ABOUT, M_QUIT, M_SUB_OFF, M_WEB,
       M_SIDEBAR, M_SHUFFLE, M_SAVE_LIST, M_REPEAT_OFF, M_REPEAT_ALL, M_REPEAT_ONE, M_REPLAYGAIN, M_EQUALIZER,
       M_TRACK = 1000, M_SPEED = 2000, M_PLAYLIST = 3000, M_CHAPTER = 4000 };

// the equalizer: ten sliders and a few presets; changes are heard at once
static void equalizer_dialog() {
  static const char *bands[] = {"31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
  static const struct { const char *name; double g[10]; } presets[] = {
      {"Flat", {0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
      {"Bass", {6, 5, 4, 2, 0, 0, 0, 0, 0, 0}},
      {"Treble", {0, 0, 0, 0, 0, 1, 2, 4, 5, 6}},
      {"Voice", {-3, -2, 0, 2, 4, 4, 3, 1, 0, -2}},
      {"Loudness", {5, 4, 2, 0, -1, 0, 1, 3, 4, 5}},
  };
  Fl_Double_Window d(460, 300, "Equalizer");
  d.set_modal();
  static Fl_Slider *sl[10];
  for (int i = 0; i < 10; i++) {
    sl[i] = new Fl_Slider(20 + i * 42, 20, 24, 200, bands[i]);
    sl[i]->type(FL_VERT_NICE_SLIDER);
    sl[i]->bounds(12, -24); // up is louder
    sl[i]->step(0.5);
    sl[i]->value(S.eq[(size_t)i]);
    sl[i]->align(FL_ALIGN_BOTTOM);
    sl[i]->labelsize(11);
    sl[i]->callback([](Fl_Widget *, void *) {
      for (int k = 0; k < 10; k++) S.eq[(size_t)k] = sl[k]->value();
      g_player->set_equalizer(S.eq);
    });
  }
  Fl_Choice *pre = new Fl_Choice(80, 252, 140, 28, "Preset:");
  for (auto &p : presets) pre->add(p.name);
  pre->callback([](Fl_Widget *w, void *) {
    int i = ((Fl_Choice *)w)->value();
    if (i < 0) return;
    for (int k = 0; k < 10; k++) {
      S.eq[(size_t)k] = presets[i].g[k];
      sl[k]->value(S.eq[(size_t)k]);
    }
    g_player->set_equalizer(S.eq);
  });
  Fl_Button *close = new Fl_Button(350, 252, 90, 28, "Close");
  close->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  d.end();
  d.show();
  while (d.shown()) Fl::wait();
  save_settings();
}

// ---- web videos: our own yt-dlp and QuickJS-ng --------------------------------------------

static void web_videos_dialog() {
  Fl_Double_Window w(520, 250, "Web Videos");
  w.set_modal();
  Fl_Box *about = new Fl_Box(16, 12, 488, 70,
                             "Videos of web sites (YouTube and many others) are played through yt-dlp. The "
                             "versions in distributions are usually too old, so the player keeps its own yt-dlp "
                             "and the QuickJS-ng JavaScript engine up to date, from their GitHub releases.");
  about->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  Fl_Check_Button *manage = new fld::CheckButton(16, 90, 488, 26, "Keep yt-dlp and QuickJS-ng up to date (downloaded)");
  webhelpers::State st = webhelpers::load_state();
  manage->value(st.manage);
  Fl_Box *info = new Fl_Box(16, 122, 488, 60);
  info->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_TOP | FL_ALIGN_WRAP);
  info->labelcolor(fld::palette().fg_dim);
  auto show_info = [info](const std::string &extra) {
    webhelpers::State s = webhelpers::load_state();
    std::string path = webhelpers::ytdlp_path();
    std::string t = path.empty() ? "No yt-dlp yet." : "Using " + path;
    if (s.manage && !s.ytdlp_version.empty())
      t += "\nyt-dlp " + s.ytdlp_version + (s.qjs_version.empty() ? "" : ", QuickJS-ng " + s.qjs_version);
    if (!extra.empty()) t += "\n" + extra;
    info->copy_label(fld::escape_at(t).c_str());
    info->redraw();
  };
  show_info(webhelpers::updating() ? "Checking for new versions…" : "");
  Fl_Button *now = new Fl_Button(16, 204, 140, 30, "Update Now");
  Fl_Return_Button *close = new Fl_Return_Button(404, 204, 100, 30, "Close");
  w.end();
  struct Ctx {
    std::function<void(const std::string &)> show;
    Fl_Button *now;
    std::shared_ptr<bool> alive;
  };
  auto alive = std::make_shared<bool>(true);
  Ctx ctx{show_info, now, alive};
  manage->callback([](Fl_Widget *b, void *d) {
    webhelpers::State s = webhelpers::load_state();
    s.manage = ((Fl_Check_Button *)b)->value() != 0;
    webhelpers::save_state(s);
    ((Ctx *)d)->show("");
  }, &ctx);
  now->callback([](Fl_Widget *b, void *d) {
    Ctx *c = (Ctx *)d;
    b->deactivate();
    c->show("Checking for new versions…");
    auto a = c->alive;
    webhelpers::update([c, a](bool ok, bool changed, const std::string &msg) {
      if (!*a) return;
      c->now->activate();
      c->show(ok ? (changed ? "Updated: " : "Up to date: ") + msg : "Failed: " + msg);
    });
  }, &ctx);
  if (webhelpers::updating()) now->deactivate();
  close->callback([](Fl_Widget *w, void *) { w->window()->hide(); });
  w.show();
  while (w.shown()) Fl::wait();
  *alive = false;
}

static void show_menu(int mx, int my) {
  g_labels.clear();
  g_tracks = g_player->tracks();
  const auto &playlist = g_player->playlist();
  g_labels.reserve(g_tracks.size() + playlist.size() + 20);
  double speed = g_player->rate();
  media::Repeat rep = g_player->repeat();
  std::vector<Fl_Menu_Item> m = {
      {"Open…", FL_CTRL + 'o', nullptr, (void *)M_OPEN},
      {"Add to Playlist…", 0, nullptr, (void *)M_ADD},
      {"Open Address…", FL_CTRL + 'u', nullptr, (void *)M_URL, FL_MENU_DIVIDER},
  };
  auto submenu = [&](const char *title, const char *type, bool off_item) {
    m.push_back({title, 0, nullptr, nullptr, FL_SUBMENU});
    size_t start = m.size();
    if (off_item) {
      bool any = false;
      for (auto &t : g_tracks) any |= t.type == type && t.selected;
      m.push_back({"Off", 0, nullptr, (void *)M_SUB_OFF, FL_MENU_RADIO | (any ? 0 : FL_MENU_VALUE)});
    }
    for (size_t i = 0; i < g_tracks.size(); i++) {
      if (g_tracks[i].type != type) continue;
      g_labels.push_back(esc(track_label(g_tracks[i])));
      m.push_back({g_labels.back().c_str(), 0, nullptr, (void *)(long)(M_TRACK + i),
                   FL_MENU_RADIO | (g_tracks[i].selected ? FL_MENU_VALUE : 0)});
    }
    if (m.size() == start) m.push_back({"(none)", 0, nullptr, nullptr, FL_MENU_INACTIVE});
    if (!strcmp(type, "sub")) {
      m.back().flags |= FL_MENU_DIVIDER;
      m.push_back({"Load Subtitles…", 0, nullptr, (void *)M_SUBS});
    }
    m.push_back({nullptr});
  };
  submenu("Video", "video", false);
  submenu("Audio", "audio", false);
  // the audio submenu ends with the sound settings
  m.pop_back();
  m.back().flags |= FL_MENU_DIVIDER;
  m.push_back({"Volume Levelling (ReplayGain)", 0, nullptr, (void *)M_REPLAYGAIN,
               FL_MENU_TOGGLE | (S.replaygain ? FL_MENU_VALUE : 0)});
  m.push_back({"Equalizer…", 0, nullptr, (void *)M_EQUALIZER, g_player->has_equalizer() ? 0 : FL_MENU_INACTIVE});
  m.push_back({nullptr});
  const auto &chapters = g_player->chapters();
  if (!chapters.empty()) {
    m.push_back({"Chapters", 0, nullptr, nullptr, FL_SUBMENU});
    double pos = g_player->position();
    for (size_t i = 0; i < chapters.size() && i < 100; i++) {
      bool here = pos >= chapters[i].start && (i + 1 == chapters.size() || pos < chapters[i + 1].start);
      std::string l = video::format_time(chapters[i].start) + "  " +
                      (chapters[i].title.empty() ? "Chapter " + std::to_string(i + 1) : chapters[i].title);
      g_labels.push_back(esc(l));
      m.push_back({g_labels.back().c_str(), 0, nullptr, (void *)(long)(M_CHAPTER + i),
                   FL_MENU_RADIO | (here ? FL_MENU_VALUE : 0)});
    }
    m.push_back({nullptr});
  }
  submenu("Subtitles", "sub", true);
  m.push_back({"Speed", 0, nullptr, nullptr, FL_SUBMENU});
  for (size_t i = 0; i < video::speeds().size(); i++) {
    char b[16];
    snprintf(b, sizeof b, "%g×", video::speeds()[i]);
    g_labels.push_back(b);
    m.push_back({g_labels.back().c_str(), 0, nullptr, (void *)(long)(M_SPEED + i),
                 FL_MENU_RADIO | (std::fabs(speed - video::speeds()[i]) < 1e-6 ? FL_MENU_VALUE : 0)});
  }
  m.push_back({nullptr});
  m.push_back({"Playlist", 0, nullptr, nullptr, FL_SUBMENU | FL_MENU_DIVIDER});
  for (size_t i = 0; i < playlist.size() && i < 50; i++) {
    std::string n = !playlist[i].title.empty() ? playlist[i].title
                    : video::is_url(playlist[i].location) ? playlist[i].location
                                                          : fld::basename(playlist[i].location);
    g_labels.push_back(esc(n));
    m.push_back({g_labels.back().c_str(), 0, nullptr, (void *)(long)(M_PLAYLIST + i),
                 FL_MENU_RADIO | ((int)i == g_player->current() ? FL_MENU_VALUE : 0)});
  }
  if (playlist.empty()) m.push_back({"(empty)", 0, nullptr, nullptr, FL_MENU_INACTIVE});
  else m.back().flags |= FL_MENU_DIVIDER;
  m.push_back({"Show Playlist", 'l', nullptr, (void *)M_SIDEBAR, FL_MENU_TOGGLE | (S.playlist ? FL_MENU_VALUE : 0)});
  m.push_back({"Save Playlist…", 0, nullptr, (void *)M_SAVE_LIST, playlist.empty() ? FL_MENU_INACTIVE : 0});
  m.push_back({nullptr});
  m.push_back({"Shuffle", 0, nullptr, (void *)M_SHUFFLE, FL_MENU_TOGGLE | (g_player->shuffle() ? FL_MENU_VALUE : 0)});
  m.push_back({"Repeat", 0, nullptr, nullptr, FL_SUBMENU});
  m.push_back({"Off", 0, nullptr, (void *)M_REPEAT_OFF, FL_MENU_RADIO | (rep == media::Repeat::Off ? FL_MENU_VALUE : 0)});
  m.push_back({"The Playlist", 0, nullptr, (void *)M_REPEAT_ALL, FL_MENU_RADIO | (rep == media::Repeat::All ? FL_MENU_VALUE : 0)});
  m.push_back({"This Item", FL_CTRL + 'r', nullptr, (void *)M_REPEAT_ONE, FL_MENU_RADIO | (rep == media::Repeat::One ? FL_MENU_VALUE : 0)});
  m.push_back({nullptr});
  m.push_back({"Resume Where I Left Off", 0, nullptr, (void *)M_RESUME,
               FL_MENU_TOGGLE | (S.resume ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER});
  m.push_back({"Take Screenshot", 's', nullptr, (void *)M_SHOT});
  m.push_back({g_fullscreen ? "Leave Full Screen" : "Full Screen", 'f', nullptr, (void *)M_FULL, FL_MENU_DIVIDER});
  m.push_back({"Web Videos…", 0, nullptr, (void *)M_WEB, FL_MENU_DIVIDER});
  m.push_back({"About", 0, nullptr, (void *)M_ABOUT});
  m.push_back({"Quit", FL_CTRL + 'q', nullptr, (void *)M_QUIT});
  m.push_back({nullptr});
  const Fl_Menu_Item *r = m[0].popup(mx, my);
  if (!r) return;
  long id = (long)r->user_data();
  if (id >= M_CHAPTER) {
    size_t i = (size_t)(id - M_CHAPTER);
    if (i < g_player->chapters().size()) g_player->seek(g_player->chapters()[i].start, true);
  } else if (id >= M_PLAYLIST) {
    g_player->play_index((int)(id - M_PLAYLIST));
  } else if (id >= M_SPEED) {
    set_speed(video::speeds()[(size_t)(id - M_SPEED)]);
  } else if (id >= M_TRACK) {
    const Player::Track &t = g_tracks[(size_t)(id - M_TRACK)];
    g_player->select_track(t.type, t.index);
  } else {
    switch (id) {
      case M_OPEN: open_dialog(false); break;
      case M_ADD: open_dialog(true); break;
      case M_URL: open_url(); break;
      case M_SUBS: add_subtitles(); break;
      case M_SUB_OFF: g_player->subtitles_off(); break;
      case M_REPLAYGAIN:
        S.replaygain = !S.replaygain;
        g_player->set_replaygain(S.replaygain);
        save_settings();
        g_surface->show_message(S.replaygain ? "Volume levelling: on (from the next track)"
                                             : "Volume levelling: off (from the next track)");
        break;
      case M_EQUALIZER: equalizer_dialog(); break;
      case M_SHOT: take_screenshot(); break;
      case M_FULL: toggle_fullscreen(); break;
      case M_LOOP: toggle_loop(); break;
      case M_SIDEBAR: toggle_playlist(); break;
      case M_SHUFFLE: set_shuffle(!g_player->shuffle()); break;
      case M_SAVE_LIST: save_playlist(); break;
      case M_REPEAT_OFF: set_repeat(media::Repeat::Off); break;
      case M_REPEAT_ALL: set_repeat(media::Repeat::All); break;
      case M_REPEAT_ONE: set_repeat(media::Repeat::One); break;
      case M_RESUME:
        S.resume = !S.resume;
        g_player->set_resume(S.resume);
        save_settings();
        break;
      case M_WEB: web_videos_dialog(); break;
      case M_ABOUT:
        fld::about_dialog("Media Player", "fl-media-player",
                          ("A video and music player built with FLTK and GStreamer.\nRendering: " +
                           std::string(g_surface->kind()))
                              .c_str());
        break;
      case M_QUIT: quit(); break;
    }
  }
}

// ---- keys and mouse ---------------------------------------------------------------------

static int global_keys(int e) {
  if (getenv("FLD_VIDEO_DEBUG") && (e == FL_SHORTCUT || e == FL_KEYBOARD))
    fprintf(stderr, "fl-media-player: key event %d key %d focus %p\n", e, Fl::event_key(), (void *)Fl::focus());
  if (e != FL_SHORTCUT) return 0;
  // let text fields in dialogs have their keys
  if (Fl::focus() && Fl::focus()->top_window() != g_win) return 0;
  int k = Fl::event_key(), st = Fl::event_state();
  bool ctrl = st & FL_CTRL, shift = st & FL_SHIFT;
  poke_bar();
  if (ctrl) {
    switch (k) {
      case 'o': open_dialog(false); return 1;
      case 'u': open_url(); return 1;
      case 'q': quit(); return 1;
      case 'r': toggle_loop(); return 1;
    }
    return 0;
  }
  switch (k) {
    case ' ': case 'k': case FL_Pause: toggle_pause(); return 1;
    case FL_Left: seek(shift ? -60 : -5); return 1;
    case FL_Right: seek(shift ? 60 : 5); return 1;
    case FL_Up: change_volume(5); return 1;
    case FL_Down: change_volume(-5); return 1;
    case 'f': case FL_F + 11: toggle_fullscreen(); return 1;
    case FL_Escape: if (g_fullscreen) { toggle_fullscreen(); return 1; } return 1; // never close on Esc
    case 'm': toggle_mute(); return 1;
    case '[': set_speed(video::next_speed(g_player->rate(), -1)); return 1;
    case ']': set_speed(video::next_speed(g_player->rate(), 1)); return 1;
    case FL_BackSpace: set_speed(1); return 1;
    case 's': take_screenshot(); return 1;
    case 'n': case FL_Page_Down: g_player->next(); return 1;
    case 'p': case FL_Page_Up: g_player->prev(); return 1;
    case 'v':
      g_player->set_subtitles_visible(!g_player->subtitles_visible());
      g_surface->show_message(g_player->subtitles_visible() ? "Subtitles: on" : "Subtitles: off");
      return 1;
    case 'j': g_player->cycle_track("sub"); return 1;
    case '#': g_player->cycle_track("audio"); return 1;
    case '.': g_player->frame_step(1); return 1;
    case ',': g_player->frame_step(-1); return 1;
    case 'i': g_surface->show_message(g_player->info(), 4); return 1;
    case 'l': toggle_playlist(); return 1;
    case FL_Menu: show_menu(g_win->w() / 2, g_win->h() / 2); return 1;
  }
  return 0;
}

static int surface_event(int e) {
  switch (e) {
    case FL_PUSH:
      if (Fl::event_button() == FL_RIGHT_MOUSE) {
        show_menu(Fl::event_x(), Fl::event_y());
        return 1;
      }
      if (Fl::event_button() == FL_LEFT_MOUSE && !g_player->has_media()) {
        open_dialog(false); // the empty player says "click"
        return 1;
      }
      if (Fl::event_button() == FL_LEFT_MOUSE) {
        if (Fl::event_clicks()) toggle_fullscreen();
        toggle_pause(); // the second click of a double click undoes the first
        return 1;
      }
      return 1;
    case FL_MOUSEWHEEL:
      change_volume(-Fl::event_dy() * 5.0);
      return 1;
    case FL_MOVE: case FL_ENTER:
      poke_bar();
      return 1;
  }
  return 0;
}

// ---- main ---------------------------------------------------------------------------------

static void replace_surface();

static void make_surface(int W, int H) {
  g_win->begin();
  g_surface = fgst::FrameView::create(0, 0, W, H - BAR, g_force_sw || g_gl_deferred);
  g_win->end();
  g_surface->set_slot(&g_player->frames());
  g_surface->on_event = surface_event;
  // software rendering: let the pipeline scale to the widget size
  g_surface->on_resize = [](int w, int h) {
    if (g_surface && g_surface->software()) g_player->set_scale_to(w, h);
  };
  if (g_surface->software()) g_player->set_scale_to(W, H - BAR);
  else g_player->set_scale_to(0, 0);
  g_surface->on_failed = [] {
    // no usable OpenGL: switch to the software renderer
    g_force_sw = true;
    replace_surface();
  };
}

static void replace_surface() {
  fgst::FrameView *old = g_surface;
  g_win->remove(old->widget());
  Fl::delete_widget(old->widget());
  make_surface(g_win->w(), g_win->h());
  g_win->insert(*g_surface->widget(), 0);
  g_surface->widget()->show();
  g_surface->start();
  layout();
  update_play_button();
}

// the first video: now the OpenGL surface
static void video_needs_gl() {
  if (!g_gl_deferred || !g_player->has_video()) return;
  g_gl_deferred = false;
  if (!g_force_sw) replace_surface();
}

int main(int argc, char **argv) {
  // run once after an install or update (the autostart entry): GStreamer
  // builds its plugin registry, which made the first start take 5 s
  if (argc == 2 && !strcmp(argv[1], "--warm-registry")) {
    // the login warm-up: skipped on small machines, where it would compete
    // with whatever is opened first
    unsigned long long ram = fld::total_ram_bytes();
    if (ram && ram < (2ull << 30)) return 0;
    fgst::init();
    return 0;
  }
  fld::init_app("fl-media-player", argc, argv);
  Fl::lock(); // Fl::awake from GStreamer threads
  load_settings();
  std::vector<std::string> files;
  char cwd[4096];
  std::string here = getcwd(cwd, sizeof cwd) ? cwd : "/";
  bool start_fs = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-media-player [--fullscreen] [--software] [FILE|URL|PLAYLIST...]\n");
      return 0;
    }
    if (a == "--fullscreen" || a == "-f") { start_fs = true; continue; }
    if (a == "--software") { g_force_sw = true; continue; }
    if (a == "--") {
      for (int j = i + 1; j < argc; j++) files.push_back(argv[j][0] == '/' || video::is_url(argv[j]) ? argv[j] : here + "/" + argv[j]);
      break;
    }
    if (a[0] == '-') continue;
    for (auto &f : video::parse_uri_list(a, here)) files.push_back(f);
  }
  const char *sw = getenv("FLD_VIDEO_SW");
  if (sw && *sw && strcmp(sw, "0")) g_force_sw = true;

  // GStreamer starts with the first file played (see Player::ensure_engine)
  g_player = new Player();
  g_player->set_replaygain(S.replaygain);
  g_player->set_equalizer(S.eq);
  g_player->set_resume(S.resume);
  g_player->set_volume(S.volume);
  g_player->set_shuffle(S.shuffle);
  g_player->set_repeat((media::Repeat)S.repeat);

  g_gl_deferred = !g_force_sw && files.empty();
  const fld::Palette &pal = fld::palette();
  const int W = 960, H = 600;
  g_win = new VideoWindow(W, H, "Media Player");
  g_win->color(FL_BLACK);
  make_surface(W, H);
  g_win->begin();
  // the playlist sidebar: the list, then its buttons
  g_side = new Fl_Group(W - SIDE, 0, SIDE, H - BAR);
  g_side->box(FL_FLAT_BOX);
  g_side->color(pal.bg2);
  g_list = new PlaylistBrowser(W - SIDE, 0, SIDE, H - BAR - 40);
  g_list->box(FL_FLAT_BOX);
  g_list->color(pal.bg2);
  g_list->textsize(13);
  static int list_cols[] = {SIDE - 70, 0};
  g_list->column_widths(list_cols);
  g_list->column_char('\t');
  g_list->callback([](Fl_Widget *w, void *) {
    Fl_Hold_Browser *b = (Fl_Hold_Browser *)w;
    if (Fl::event_clicks() && Fl::event() == FL_RELEASE && b->value() > 0) g_player->play_index(b->value() - 1);
  });
  auto side_btn = [&](const char *icon, const char *fallback, const char *tip, Fl_Callback *cb) {
    fld::FlatButton *b = new fld::FlatButton(0, 0, 34, 34);
    set_icon(b, icon, fallback);
    b->tooltip(tip);
    b->callback(cb);
    b->clear_visible_focus();
    return b;
  };
  side_btn("list-add", "+", "Add files", [](Fl_Widget *, void *) { open_dialog(true); });
  side_btn("list-remove", "−", "Remove (Delete)", [](Fl_Widget *, void *) {
    if (g_list->value() > 0) g_player->remove(g_list->value() - 1);
  });
  side_btn("go-up", "↑", "Move up", [](Fl_Widget *, void *) {
    int i = g_list->value() - 1;
    if (i > 0) {
      g_player->move(i, i - 1);
      g_list->select(i);
    }
  });
  side_btn("go-down", "↓", "Move down", [](Fl_Widget *, void *) {
    int i = g_list->value() - 1;
    if (i >= 0 && i + 1 < g_player->count()) {
      g_player->move(i, i + 1);
      g_list->select(i + 2);
    }
  });
  g_shuffle_btn = side_btn("media-playlist-shuffle", "Mix", "Shuffle", [](Fl_Widget *, void *) {
    set_shuffle(!g_player->shuffle());
  });
  g_shuffle_btn->type(FL_TOGGLE_BUTTON);
  g_repeat_btn = side_btn("media-playlist-repeat", "Rep", "Repeat", [](Fl_Widget *, void *) {
    set_repeat(media::next_repeat(g_player->repeat()));
  });
  g_repeat_btn->type(FL_TOGGLE_BUTTON);
  side_btn("document-save", "Save", "Save the playlist", [](Fl_Widget *, void *) { save_playlist(); });
  side_btn("open-menu", "☰", "More", [](Fl_Widget *w, void *) { playlist_menu(w->x(), w->y()); });
  g_side->end();
  g_side->resizable(nullptr);
  g_bar = new Fl_Group(0, H - BAR, W, BAR);
  g_bar->box(FL_FLAT_BOX);
  g_bar->color(pal.dark ? pal.bg : fl_rgb_color(0x23, 0x25, 0x2a));
  Fl_Color bar_fg = FL_WHITE;
  auto button = [&](const char *tip, Fl_Callback *cb) {
    fld::FlatButton *b = new fld::FlatButton(0, H - BAR + 5, 36, 36);
    b->color(g_bar->color());
    b->labelcolor(bar_fg);
    b->labelsize(16);
    b->tooltip(tip);
    b->callback(cb);
    b->clear_visible_focus();
    return b;
  };
  g_play = button("Play (Space)", [](Fl_Widget *, void *) { toggle_pause(); });
  g_prev = button("Previous (P)", [](Fl_Widget *, void *) { g_player->prev(); });
  set_icon(g_prev, "media-skip-backward", "⏮");
  g_next = button("Next (N)", [](Fl_Widget *, void *) { g_player->next(); });
  set_icon(g_next, "media-skip-forward", "⏭");
  g_seek = new Fl_Slider(0, 0, 100, 18);
  g_seek->type(FL_HOR_FILL_SLIDER);
  g_seek->box(FL_FLAT_BOX);
  g_seek->color(fl_color_average(g_bar->color(), FL_WHITE, 0.75f));
  g_seek->selection_color(pal.accent);
  g_seek->tooltip("Seek (Left/Right: 5 s, Shift: 1 min)");
  g_seek->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
  g_seek->clear_visible_focus();
  g_seek->callback([](Fl_Widget *w, void *) {
    Fl_Slider *s = (Fl_Slider *)w;
    g_seeking = Fl::pushed() == s;
    // keyframe seeks while dragging, an exact one on release
    g_player->seek(s->value(), !g_seeking);
  });
  g_time = new Fl_Box(0, 0, 130, 36, "--:-- / --:--");
  g_time->labelcolor(bar_fg);
  g_time->labelsize(13);
  g_time->align(FL_ALIGN_INSIDE | FL_ALIGN_RIGHT);
  g_mute = button("Mute (M)", [](Fl_Widget *, void *) { toggle_mute(); });
  g_vol = new Fl_Slider(0, 0, 90, 16);
  g_vol->type(FL_HOR_FILL_SLIDER);
  g_vol->box(FL_FLAT_BOX);
  g_vol->color(fl_color_average(g_bar->color(), FL_WHITE, 0.75f));
  g_vol->selection_color(pal.accent);
  g_vol->range(0, 130);
  g_vol->step(1);
  g_vol->value(S.volume);
  g_vol->tooltip("Volume (Up/Down, mouse wheel)");
  g_vol->clear_visible_focus();
  g_vol->callback([](Fl_Widget *w, void *) {
    S.volume = ((Fl_Slider *)w)->value();
    g_player->set_volume(S.volume);
    g_player->set_mute(false);
    update_volume();
  });
  g_list_btn = button("Playlist (L)", [](Fl_Widget *, void *) { toggle_playlist(); });
  set_icon(g_list_btn, "view-list", "☰");
  g_list_btn->type(FL_TOGGLE_BUTTON);
  g_list_btn->value(S.playlist);
  g_full = button("Full screen (F, double-click)", [](Fl_Widget *, void *) { toggle_fullscreen(); });
  set_icon(g_full, "view-fullscreen", "⛶");
  fld::FlatButton *menu = button("Menu (right-click)", [](Fl_Widget *w, void *) { show_menu(w->x(), w->y()); });
  set_icon(menu, "open-menu", "☰");
  if (!fld::icon("open-menu", 20) && fld::icon("preferences-system", 20)) set_icon(menu, "preferences-system", "☰");
  g_bar->end();
  g_win->end();
  g_win->resizable(g_win); // layout() places everything
  g_win->size_range(360, 240);
  g_win->callback([](Fl_Widget *, void *) {
    if (Fl::event() == FL_SHORTCUT && Fl::event_key() == FL_Escape) return;
    quit();
  });
  fld::set_window_icon(g_win, "fl-media-player");
  Fl::add_handler(global_keys);

  g_player->frames().on_frame = [] { g_surface->redraw(); };
  g_player->on_position = [] { update_time(); };
  g_player->on_state = [] {
    update_play_button();
    update_time();
    update_mpris();
  };
  g_player->on_playlist = [] { fill_playlist(); update_title(); };
  g_player->on_tags = [] {
    fill_playlist();
    update_play_button();
    update_mpris();
  };
  g_player->on_media = [] {
    video_needs_gl();
    update_title();
    update_time();
    update_play_button();
  };
  g_player->on_tracks = [] {
    video_needs_gl();
    update_play_button();
  };
  g_player->on_buffering = [](int pct) {
    if (pct < 100) g_surface->show_message("Buffering " + std::to_string(pct) + "%", 1);
  };
  g_player->on_error = [](const std::string &msg) {
    std::string m = "Cannot play this: " + msg;
    g_surface->show_message(m, 6);
    g_win->copy_label(m.c_str());
    update_play_button();
  };

  layout();
  update_volume();
  update_play_button();
  update_title();
  update_order_buttons();
  start_mpris();
  // our yt-dlp and QuickJS-ng: fetched once a day, quietly
  if (webhelpers::update_due()) webhelpers::update([](bool, bool, const std::string &msg) {
    if (!msg.empty()) fprintf(stderr, "fl-media-player: web video helpers: %s\n", msg.c_str());
  });
  g_win->show();
  g_surface->start();
  if (start_fs) toggle_fullscreen();
  if (!files.empty()) open_files(files, false);
  int r = Fl::run();
  delete g_player;
  return r;
}
