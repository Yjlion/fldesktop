// fl-video: video and audio player for fldesktop, built on GStreamer
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_File_Chooser.H>
#include <FL/Fl_Menu_Item.H>
#include <FL/Fl_Slider.H>
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
#include "video_util.h"

static const int BAR = 46;

struct Settings {
  double volume = 100;
  bool resume = true;
} S;

static std::string settings_path() { return fld::fld_config_dir() + "/video.ini"; }

static void load_settings() {
  fld::Ini c;
  c.load(settings_path());
  S.volume = std::max(0.0, std::min(130.0, c.get_double("Video", "Volume", 100)));
  S.resume = c.get_bool("Video", "ResumePlayback", true);
}

static void save_settings() {
  fld::Ini c;
  c.load(settings_path());
  c.set_int("Video", "Volume", (int)std::lround(S.volume));
  c.set_bool("Video", "ResumePlayback", S.resume);
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
static bool g_seeking = false;

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
           muted ? "🔇" : "🔊");
  if (Fl::pushed() != g_vol) g_vol->value(v);
  g_mute->tooltip(muted ? "Unmute (M)" : "Mute (M)");
}

static void update_play_button() {
  bool paused = g_player->paused() || g_player->at_end();
  set_icon(g_play, paused ? "media-playback-start" : "media-playback-pause", paused ? "▶" : "⏸");
  g_play->tooltip(paused ? "Play (Space)" : "Pause (Space)");
  bool playing = !paused && g_player->has_media();
  idle::inhibit(g_win ? (Fl_Window *)g_win : nullptr, playing && g_player->has_video());
  // audio only: a placeholder with the title instead of a black picture
  std::string ph = g_player->has_media() && !g_player->has_video() ? g_player->title() : "";
  if (ph != g_surface->osd().placeholder) {
    g_surface->osd().placeholder = ph;
    g_surface->redraw();
  }
}

static void update_title() {
  std::string t = g_player->title();
  long n = g_player->count(), i = g_player->current();
  if (n > 1 && i >= 0) t += " (" + std::to_string(i + 1) + "/" + std::to_string(n) + ")";
  t = t.empty() ? "Video Player" : t + " – Video Player";
  extern void set_window_title(const std::string &);
  set_window_title(t);
  if (n > 1) { g_prev->activate(); g_next->activate(); }
  else { g_prev->deactivate(); g_next->deactivate(); }
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
  g_surface->widget()->resize(0, 0, W, std::max(1, vh));
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
  g_full->resize(right - 36, y, 36, 36); right -= 44;
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
  g_win->hide();
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
  g_player->set_loop(!g_player->loop());
  g_surface->show_message(g_player->loop() ? "Repeat: on" : "Repeat: off");
}

static void toggle_mute() {
  g_player->set_mute(!g_player->muted());
  update_volume();
}

enum { M_OPEN = 1, M_ADD, M_URL, M_SUBS, M_SHOT, M_FULL, M_RESUME, M_LOOP, M_ABOUT, M_QUIT, M_SUB_OFF,
       M_TRACK = 1000, M_SPEED = 2000, M_PLAYLIST = 3000 };

static void show_menu(int mx, int my) {
  g_labels.clear();
  g_tracks = g_player->tracks();
  const auto &playlist = g_player->playlist();
  g_labels.reserve(g_tracks.size() + playlist.size() + 20);
  double speed = g_player->rate();
  bool loop = g_player->loop();
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
  m.push_back({nullptr});
  m.push_back({"Repeat", FL_CTRL + 'r', nullptr, (void *)M_LOOP, FL_MENU_TOGGLE | (loop ? FL_MENU_VALUE : 0)});
  m.push_back({"Resume Where I Left Off", 0, nullptr, (void *)M_RESUME,
               FL_MENU_TOGGLE | (S.resume ? FL_MENU_VALUE : 0) | FL_MENU_DIVIDER});
  m.push_back({"Take Screenshot", 's', nullptr, (void *)M_SHOT});
  m.push_back({g_fullscreen ? "Leave Full Screen" : "Full Screen", 'f', nullptr, (void *)M_FULL, FL_MENU_DIVIDER});
  m.push_back({"About", 0, nullptr, (void *)M_ABOUT});
  m.push_back({"Quit", FL_CTRL + 'q', nullptr, (void *)M_QUIT});
  m.push_back({nullptr});
  const Fl_Menu_Item *r = m[0].popup(mx, my);
  if (!r) return;
  long id = (long)r->user_data();
  if (id >= M_PLAYLIST) {
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
      case M_SHOT: take_screenshot(); break;
      case M_FULL: toggle_fullscreen(); break;
      case M_LOOP: toggle_loop(); break;
      case M_RESUME:
        S.resume = !S.resume;
        g_player->set_resume(S.resume);
        save_settings();
        break;
      case M_ABOUT:
        fld::about_dialog("Video Player", "fl-video",
                          ("A video and audio player built with FLTK and GStreamer.\nRendering: " +
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
    fprintf(stderr, "fl-video: key event %d key %d focus %p\n", e, Fl::event_key(), (void *)Fl::focus());
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

static void make_surface(int W, int H) {
  g_win->begin();
  g_surface = fgst::FrameView::create(0, 0, W, H - BAR, g_force_sw);
  g_win->end();
  g_surface->set_slot(&g_player->frames());
  g_surface->on_event = surface_event;
  // software rendering: let the pipeline scale to the widget size
  g_surface->on_resize = [](int w, int h) {
    if (g_surface && g_surface->software()) fgst::set_scale_to(g_player->video_sink(), w, h);
  };
  if (g_surface->software()) fgst::set_scale_to(g_player->video_sink(), W, H - BAR);
  else fgst::set_scale_to(g_player->video_sink(), 0, 0);
  g_surface->on_failed = [] {
    // no usable OpenGL: switch to the software renderer
    fgst::FrameView *old = g_surface;
    g_win->remove(old->widget());
    Fl::delete_widget(old->widget());
    g_force_sw = true;
    make_surface(g_win->w(), g_win->h());
    g_win->insert(*g_surface->widget(), 0);
    g_surface->widget()->show();
    g_surface->start();
    layout();
    update_play_button();
  };
}

int main(int argc, char **argv) {
  fld::init_app("fl-video", argc, argv);
  Fl::lock(); // Fl::awake from GStreamer threads
  load_settings();
  std::vector<std::string> files;
  char cwd[4096];
  std::string here = getcwd(cwd, sizeof cwd) ? cwd : "/";
  bool start_fs = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-h" || a == "--help") {
      printf("Usage: fl-video [--fullscreen] [--software] [FILE|URL...]\n");
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

  g_player = new Player();
  if (!g_player->ok()) {
    fl_alert("Could not start the GStreamer playback engine:\n%s", g_player->init_error().c_str());
    return 1;
  }
  g_player->set_resume(S.resume);
  g_player->set_volume(S.volume);

  const fld::Palette &pal = fld::palette();
  const int W = 960, H = 600;
  g_win = new VideoWindow(W, H, "Video Player");
  g_win->color(FL_BLACK);
  make_surface(W, H);
  g_win->begin();
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
  fld::set_window_icon(g_win, "fl-video");
  Fl::add_handler(global_keys);

  g_player->frames().on_frame = [] { g_surface->redraw(); };
  g_player->on_position = [] { update_time(); };
  g_player->on_state = [] {
    update_play_button();
    update_time();
  };
  g_player->on_media = [] {
    update_title();
    update_time();
    update_play_button();
  };
  g_player->on_tracks = [] { update_play_button(); };
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
  g_win->show();
  g_surface->start();
  if (start_fs) toggle_fullscreen();
  if (!files.empty()) open_files(files, false);
  int r = Fl::run();
  delete g_player;
  return r;
}
