// fl-panel: actions for the media, volume and brightness keys.
//
// fl-wm binds the keys to "fl-panel --action <name>"; that process hands the
// action to the running panel over a datagram socket in $XDG_RUNTIME_DIR and
// exits, and the panel does it and shows the on-screen display. Without a
// panel the process does the action itself (no display).
//
//   volume-up, volume-down, volume-mute, mic-mute
//   brightness-up, brightness-down
//   kbd-brightness-up, kbd-brightness-down, kbd-brightness-toggle
//   media-play-pause, media-play, media-pause, media-stop, media-next,
//   media-previous
//   caps-lock on|off, num-lock on|off, layout <name>   (the display only)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "fld/util.h"
#include "panel.h"
#ifdef FLD_HAVE_BACKLIGHT
#include "backlight.h"
#endif
#ifdef FLD_HAVE_MPRIS
#include "mpris.h"
#endif

namespace {

bool g_osd = true; // [Panel] OSD=false turns the display off

bool known(const std::string &a) {
  static const char *names[] = {"volume-up", "volume-down", "volume-mute", "mic-mute", "brightness-up",
                                "brightness-down", "kbd-brightness-up", "kbd-brightness-down", "kbd-brightness-toggle",
                                "media-play-pause", "media-play", "media-pause", "media-stop", "media-next",
                                "media-previous", "caps-lock", "num-lock", "layout"};
  std::string name = a.substr(0, a.find(' '));
  for (const char *n : names)
    if (name == n) return true;
  return false;
}

// "media-play-pause" -> "PlayPause"
std::string mpris_method(const std::string &a) {
  if (a == "media-play-pause") return "PlayPause";
  if (a == "media-play") return "Play";
  if (a == "media-pause") return "Pause";
  if (a == "media-stop") return "Stop";
  if (a == "media-next") return "Next";
  if (a == "media-previous") return "Previous";
  return "";
}

const char *media_icon(const std::string &method) {
  if (method == "Next") return "media-skip-forward";
  if (method == "Previous") return "media-skip-backward";
  if (method == "Stop") return "media-playback-stop";
  if (method == "Pause") return "media-playback-pause";
  return "media-playback-start";
}

void show(const std::string &icon, int level, const std::string &text = "") {
  if (g_osd) osd_show(icon, level, text);
}

void brightness(int dir) {
#ifdef FLD_HAVE_BACKLIGHT
  int pct;
  std::string err;
  if (bl::screen_step(dir, pct, err)) show("display-brightness", pct);
  else fprintf(stderr, "fl-panel: brightness: %s\n", err.c_str());
#else
  (void)dir;
#endif
}

void kbd_brightness(int dir, bool wrap) {
#ifdef FLD_HAVE_BACKLIGHT
  int pct;
  if (bl::kbd_step(dir, wrap, pct)) show("keyboard-brightness", pct, "Keyboard light");
#else
  (void)dir;
  (void)wrap;
#endif
}

int fd_listen = -1;

void on_datagram(int fd, void *) {
  char buf[512];
  ssize_t n = recv(fd, buf, sizeof buf - 1, MSG_DONTWAIT);
  if (n <= 0) return;
  buf[n] = 0;
  std::string a = fld::trim(buf);
  if (known(a)) panel_run_action(a);
}

// the headless volume commands (wpctl for PipeWire, pactl for PulseAudio)
int volume_cli(const std::string &a) {
  std::string out;
  bool wp = !fld::which("wpctl").empty(), pa = !wp && !fld::which("pactl").empty();
  if (!wp && !pa) return 1;
  if (a == "volume-up")
    return wp ? fld::run_capture({"wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+"}, out)
              : fld::run_capture({"pactl", "set-sink-volume", "@DEFAULT_SINK@", "+5%"}, out);
  if (a == "volume-down")
    return wp ? fld::run_capture({"wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-"}, out)
              : fld::run_capture({"pactl", "set-sink-volume", "@DEFAULT_SINK@", "-5%"}, out);
  if (a == "volume-mute")
    return wp ? fld::run_capture({"wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle"}, out)
              : fld::run_capture({"pactl", "set-sink-mute", "@DEFAULT_SINK@", "toggle"}, out);
  if (a == "mic-mute")
    return wp ? fld::run_capture({"wpctl", "set-mute", "@DEFAULT_AUDIO_SOURCE@", "toggle"}, out)
              : fld::run_capture({"pactl", "set-source-mute", "@DEFAULT_SOURCE@", "toggle"}, out);
  return 1;
}

} // namespace

std::string panel_socket_path() {
  const char *dir = getenv("XDG_RUNTIME_DIR");
  const char *wd = getenv("WAYLAND_DISPLAY");
  std::string d = dir && *dir ? dir : "/tmp";
  return d + "/fldesktop-panel-" + (wd && *wd && !strchr(wd, '/') ? wd : "wayland-0") + ".sock";
}

void panel_actions_listen(bool osd) {
  g_osd = osd;
  std::string path = panel_socket_path();
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) return;
  strcpy(addr.sun_path, path.c_str());
  fd_listen = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
  if (fd_listen < 0) return;
  unlink(path.c_str()); // the previous panel's (one panel per session)
  if (bind(fd_listen, (sockaddr *)&addr, sizeof addr) != 0) {
    perror("fl-panel: action socket");
    close(fd_listen);
    fd_listen = -1;
    return;
  }
  Fl::add_fd(fd_listen, FL_READ, on_datagram);
  // a panel that is gone takes its socket along, so --action works alone
  static std::string s_path = path;
  atexit([] { unlink(s_path.c_str()); });
#ifdef FLD_HAVE_MPRIS
  mpris::Client::get().start();
#endif
}

void panel_run_action(const std::string &a) {
  std::string name = a.substr(0, a.find(' '));
  std::string arg = a.find(' ') == std::string::npos ? "" : fld::trim(a.substr(a.find(' ') + 1));
  if (name == "volume-up") return panel_volume_step(5);
  if (name == "volume-down") return panel_volume_step(-5);
  if (name == "volume-mute") return panel_volume_toggle_mute();
  if (name == "mic-mute") return panel_mic_toggle_mute();
  if (name == "brightness-up") return brightness(1);
  if (name == "brightness-down") return brightness(-1);
  if (name == "kbd-brightness-up") return kbd_brightness(1, false);
  if (name == "kbd-brightness-down") return kbd_brightness(-1, false);
  if (name == "kbd-brightness-toggle") return kbd_brightness(1, true);
  if (name == "caps-lock") return show("input-keyboard", -1, arg == "on" ? "Caps Lock on" : "Caps Lock off");
  if (name == "num-lock") return show("input-keyboard", -1, arg == "on" ? "Num Lock on" : "Num Lock off");
  if (name == "layout") return show("input-keyboard", -1, arg);
  std::string method = mpris_method(name);
  if (!method.empty()) {
#ifdef FLD_HAVE_MPRIS
    mpris::Client &c = mpris::Client::get();
    const mpris::Player *p = mpris::active(c.state());
    if (!p) return;
    std::string what = mpris::label(*p);
    c.command(method);
    // play/pause: the state it goes to
    std::string icon = media_icon(method);
    if (method == "PlayPause") icon = p->status == "Playing" ? "media-playback-pause" : "media-playback-start";
    show(icon, -1, what);
#endif
  }
}

int panel_action_main(const std::string &action) {
  if (!known(action)) {
    fprintf(stderr, "fl-panel: unknown action '%s'\n", action.c_str());
    return 2;
  }
  // the running panel does it (and shows it)
  std::string path = panel_socket_path();
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() < sizeof addr.sun_path) {
    strcpy(addr.sun_path, path.c_str());
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      bool sent = sendto(fd, action.data(), action.size(), 0, (sockaddr *)&addr, sizeof addr) == (ssize_t)action.size();
      close(fd);
      if (sent) return 0;
    }
  }
  // no panel: do it here, without the display
  std::string name = action.substr(0, action.find(' '));
  if (name.compare(0, 6, "volume") == 0 || name == "mic-mute") return volume_cli(name);
#ifdef FLD_HAVE_BACKLIGHT
  int pct;
  std::string err;
  if (name == "brightness-up" || name == "brightness-down") {
    if (bl::screen_step(name == "brightness-up" ? 1 : -1, pct, err)) return 0;
    fprintf(stderr, "fl-panel: %s\n", err.c_str());
    return 1;
  }
  if (name.compare(0, 14, "kbd-brightness") == 0)
    return bl::kbd_step(name == "kbd-brightness-down" ? -1 : 1, name == "kbd-brightness-toggle", pct) ? 0 : 1;
#endif
#ifdef FLD_HAVE_MPRIS
  std::string method = mpris_method(name);
  if (!method.empty()) return mpris::command_sync(method) ? 0 : 1;
#endif
  return 0; // caps-lock, layout: only shown
}
