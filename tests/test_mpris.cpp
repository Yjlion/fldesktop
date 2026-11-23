// fld-mpris: which player the keys act on, labels; with FLD_MPRIS_MOCK the
// mocked player on the session bus (mpris-mock.sh)
#include <cstdlib>

#include "mpris.h"
#include "test.h"

int main() {
  using namespace mpris;
  CHECK_EQ(short_name("org.mpris.MediaPlayer2.vlc"), std::string("vlc"));
  CHECK_EQ(short_name("org.mpris.MediaPlayer2.firefox.instance_1_42"), std::string("firefox"));
  CHECK_EQ(short_name("org.mpris.MediaPlayer2.fl-media-player.instance42"), std::string("fl-media-player"));
  State s;
  CHECK(active(s) == nullptr);
  Player a, b, c;
  a.bus = "a";
  a.status = "Paused";
  a.changed = 500;
  b.bus = "b";
  b.status = "Stopped";
  b.changed = 100;
  s.players = {a, b};
  CHECK(active(s)->bus == "a"); // played last
  c.bus = "c";
  c.status = "Playing";
  c.changed = 50;
  s.players.push_back(c);
  CHECK(active(s)->bus == "c"); // playing wins
  s.players[0].status = "Playing";
  CHECK(active(s)->bus == "a"); // both playing: the newer
  Player p;
  p.identity = "VLC";
  CHECK_EQ(label(p), std::string("VLC"));
  p.title = "Song";
  CHECK_EQ(label(p), std::string("Song"));
  p.artist = "Band";
  CHECK_EQ(label(p), std::string("Song \xe2\x80\x93 Band"));
  if (getenv("FLD_MPRIS_MOCK")) {
    CHECK(command_sync("PlayPause"));
    CHECK(command_sync("Next"));
  }
  return test_result("mpris");
}
