// fl-media-player: gapless playback. Two short WAV files play through the
// real Player (GStreamer playbin, a fakesink for the sound): the second
// starts in the same pipeline (about-to-finish), not after an end of stream.
#include <FL/Fl.H>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "fld/util.h"
#include "gst_player.h"
#include "test.h"

// 16-bit mono PCM, `secs` of a sine at `hz`
static void write_wav(const std::string &path, double secs, double hz) {
  const int rate = 22050;
  int n = (int)(secs * rate);
  std::string d;
  auto u32 = [&](uint32_t v) { d.append((const char *)&v, 4); };
  auto u16 = [&](uint16_t v) { d.append((const char *)&v, 2); };
  d += "RIFF";
  u32(36 + n * 2);
  d += "WAVEfmt ";
  u32(16), u16(1), u16(1), u32(rate), u32(rate * 2), u16(2), u16(16);
  d += "data";
  u32(n * 2);
  for (int i = 0; i < n; i++) u16((uint16_t)(int16_t)(8000 * std::sin(2 * M_PI * hz * i / rate)));
  fld::write_file(path, d);
}

int main() {
  setenv("FLD_AUDIO_SINK", "fakesink sync=true", 1);
  char tmpl[] = "/tmp/fld-gapless-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  setenv("XDG_CACHE_HOME", dir.c_str(), 1);  // resume positions
  std::string a = dir + "/a.wav", b = dir + "/b.wav";
  write_wav(a, 0.8, 440);
  write_wav(b, 0.8, 660);

  Player p;
  if (!p.ensure_engine()) {
    printf("no GStreamer playbin: %s\n", p.init_error().c_str());
    return 77;
  }
  std::vector<int> started;
  p.on_media = [&] {
    if (started.empty() || started.back() != p.current()) started.push_back(p.current());
  };
  std::string error;
  p.on_error = [&](const std::string &e) { error = e; };
  p.load(a, false);
  p.load(b, true);
  for (int i = 0; i < 200 && !p.at_end() && error.empty(); i++) Fl::wait(0.05);
  CHECK(error.empty());
  if (!error.empty()) printf("error: %s\n", error.c_str());
  CHECK(p.at_end());
  CHECK_EQ(p.current(), 1);
  CHECK(started.size() >= 2 && started[0] == 0 && started[1] == 1);
  CHECK_EQ(p.gapless_switches(), 1);  // b came through about-to-finish

  // without gapless the same list still plays through (end of stream, then the next)
  p.set_gapless(false);
  p.play_index(0);
  for (int i = 0; i < 200 && !(p.at_end() && p.current() == 1) && error.empty(); i++) Fl::wait(0.05);
  CHECK(p.at_end());
  CHECK_EQ(p.gapless_switches(), 1);  // no more gapless switches
  std::string out;
  fld::run_capture({"rm", "-rf", dir}, out);
  return test_result("gapless");
}
