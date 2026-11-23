// MPRIS media players on the session bus (GDBus on the fld-glib thread):
// what is playing and the play/pause/next/previous buttons, for the panel's
// volume popup and the media keys.
//
// The UI sees plain snapshots (State); call Fl::lock() once in main().
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace mpris {

struct Player {
  std::string bus;      // org.mpris.MediaPlayer2.vlc
  std::string identity; // "VLC media player"
  std::string desktop;  // vlc (its .desktop file)
  std::string status;   // Playing, Paused, Stopped
  std::string title, artist, album, art_url;
  bool can_play = false, can_pause = false, can_next = false, can_prev = false;
  long long changed = 0; // when it last started playing (monotonic ms)
};

struct State {
  std::vector<Player> players;
};

// The player the buttons and keys act on: one that plays, else the one that
// played last; null without players.
const Player *active(const State &s);
// "Title – Artist", "Title", the player's name
std::string label(const Player &p);
// The bus name's part after org.mpris.MediaPlayer2 without the instance
// (".instance1234"): "vlc" for org.mpris.MediaPlayer2.vlc.instance42
std::string short_name(const std::string &bus);

class Client {
public:
  static Client &get();
  void start();
  const State &state() const { return state_; }
  std::function<void()> on_changed; // FLTK thread
  // PlayPause, Play, Pause, Stop, Next, Previous on the active player
  void command(const std::string &method);

  State state_; // internal: written in the FLTK thread only
private:
  Client() = default;
};

// Without the panel (fl-panel --action): the same on the session bus, now.
// False when no player answered.
bool command_sync(const std::string &method);

} // namespace mpris
