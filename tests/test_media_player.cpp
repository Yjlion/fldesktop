#include <algorithm>
#include "test.h"
#include <unistd.h>
#include "fld/util.h"
#include <cstdlib>
#include "fld/ini.h"
#include "playlist_util.h"
#include "video_util.h"

using namespace video;

int main() {
  CHECK_EQ(format_time(0), std::string("0:00"));
  CHECK_EQ(format_time(75.9), std::string("1:15"));
  CHECK_EQ(format_time(3725), std::string("1:02:05"));
  CHECK_EQ(format_time(-1), std::string("--:--"));
  CHECK_EQ(format_time(0.0 / 0.0), std::string("--:--"));

  CHECK(is_url("https://example.org/a.mp4"));
  CHECK(is_url("rtsp://cam/stream"));
  CHECK(!is_url("/home/u/a.mp4"));
  CHECK(!is_url("://x"));
  CHECK(!is_url("a b://x"));

  auto v = parse_uri_list("file:///home/u/My%20Video.mkv\r\n# comment\n\nhttps://h/x.m3u8\nrel/clip.mp4\n"
                          "file://localhost/tmp/a.ogg\n/abs/b.webm",
                          "/cwd");
  CHECK_EQ(v.size(), (size_t)5);
  CHECK_EQ(v[0], std::string("/home/u/My Video.mkv"));
  CHECK_EQ(v[1], std::string("https://h/x.m3u8"));
  CHECK_EQ(v[2], std::string("/cwd/rel/clip.mp4"));
  CHECK_EQ(v[3], std::string("/tmp/a.ogg"));
  CHECK_EQ(v[4], std::string("/abs/b.webm"));
  CHECK_EQ(parse_uri_list("x", "/")[0], std::string("/x"));

  CHECK(is_subtitle("/a/Movie.EN.SRT"));
  CHECK(is_subtitle("b.ass"));
  CHECK(!is_subtitle("c.mkv"));
  CHECK(!is_subtitle("noext"));

  CHECK_EQ(next_speed(1.0, 1), 1.25);
  CHECK_EQ(next_speed(1.0, -1), 0.75);
  CHECK_EQ(next_speed(4.0, 1), 4.0);
  CHECK_EQ(next_speed(0.25, -1), 0.25);
  CHECK_EQ(next_speed(1.1, 1), 1.25);
  CHECK_EQ(next_speed(1.1, -1), 1.0);

  // URIs for GStreamer
  CHECK_EQ(to_uri("/home/u/My Videos/a#1.mkv"), std::string("file:///home/u/My%20Videos/a%231.mkv"));
  CHECK_EQ(to_uri("/tmp/ä.ogg"), std::string("file:///tmp/%C3%A4.ogg"));
  CHECK_EQ(to_uri("https://example.org/v.webm"), std::string("https://example.org/v.webm"));
  CHECK(needs_resolver("https://www.youtube.com/watch?v=x"));
  CHECK(needs_resolver("https://youtu.be/x"));
  CHECK(!needs_resolver("https://example.org/v.webm"));
  CHECK(!needs_resolver("/home/u/youtube.com.mkv"));

  // resume positions
  fld::Ini ini;
  CHECK_EQ(resume_get(ini, "/v/a.mkv"), 0.0);
  resume_put(ini, "/v/a.mkv", 125.5, 600);
  CHECK_EQ(resume_get(ini, "/v/a.mkv"), 125.5);
  CHECK_EQ(resume_get(ini, "/v/b.mkv"), 0.0);
  resume_put(ini, "/v/a.mkv", 595, 600); // near the end: forget it
  CHECK_EQ(resume_get(ini, "/v/a.mkv"), 0.0);
  resume_put(ini, "/v/a.mkv", 60, 600);
  resume_put(ini, "/v/a.mkv", 2, 600); // near the start: forget it
  CHECK_EQ(resume_get(ini, "/v/a.mkv"), 0.0);
  resume_put(ini, "https://x/y", 30, -1); // streams without a duration
  CHECK_EQ(resume_get(ini, "https://x/y"), 30.0);
  CHECK(resume_key("/v/a.mkv") != resume_key("/v/b.mkv"));
  CHECK_EQ(resume_key("/v/a.mkv").size(), (size_t)16);
  // subtitles next to a video
  {
    std::string dir = "/tmp/fld-test-video-" + std::to_string(getpid());
    fld::mkdir_p(dir);
    fld::write_file(dir + "/film.mkv", "");
    CHECK_EQ(find_subtitle(dir + "/film.mkv"), std::string());
    fld::write_file(dir + "/film.en.srt", "");
    CHECK_EQ(find_subtitle(dir + "/film.mkv"), dir + "/film.en.srt");
    fld::write_file(dir + "/film.srt", "");
    CHECK_EQ(find_subtitle(dir + "/film.mkv"), dir + "/film.srt");
    CHECK_EQ(find_subtitle("https://x/film.mkv"), std::string());
    if (system(("rm -rf '" + dir + "'").c_str()) != 0) fprintf(stderr, "cleanup failed\n");
  }
  // playlists (fl-media-player)
  {
    using namespace media;
    CHECK(is_playlist("a/Mix.M3U"));
    CHECK(is_playlist("x.m3u8"));
    CHECK(is_playlist("radio.pls"));
    CHECK(!is_playlist("song.mp3"));
    auto m = parse_playlist("#EXTM3U\n#EXTINF:215,Band - Song\nmusic/song.mp3\n\n# comment\n"
                            "file:///home/u/My%20Song.ogg\nhttp://radio/stream\r\nC:\\Music\\x.mp3\n",
                            "/lists", false);
    CHECK_EQ(m.size(), (size_t)4);
    CHECK_EQ(m[0].location, std::string("/lists/music/song.mp3"));
    CHECK_EQ(m[0].title, std::string("Band - Song"));
    CHECK_NEAR(m[0].length, 215.0);
    CHECK_EQ(m[1].location, std::string("/home/u/My Song.ogg"));
    CHECK_EQ(m[1].title, std::string(""));
    CHECK_EQ(m[2].location, std::string("http://radio/stream"));
    CHECK_EQ(m[3].location, std::string("/lists/C:/Music/x.mp3"));
    auto p = parse_playlist("[playlist]\nFile2=b.ogg\nFile1=http://a/stream\nTitle1=Radio\nLength1=-1\n"
                            "NumberOfEntries=2\nVersion=2\n", "/d", true);
    CHECK_EQ(p.size(), (size_t)2);
    CHECK_EQ(p[0].location, std::string("http://a/stream"));
    CHECK_EQ(p[0].title, std::string("Radio"));
    CHECK_EQ(p[1].location, std::string("/d/b.ogg"));
    // written and read back
    std::vector<Entry> es = {{"/m/a.mp3", "A", 61}, {"http://s/x", "", -1}};
    auto back = parse_playlist(write_m3u(es), "/", false);
    CHECK_EQ(back.size(), (size_t)2);
    CHECK_EQ(back[0].title, std::string("A"));
    CHECK_NEAR(back[0].length, 61.0);
    CHECK_EQ(back[1].location, std::string("http://s/x"));
    auto back2 = parse_playlist(write_pls(es), "/", true);
    CHECK_EQ(back2.size(), (size_t)2);
    CHECK_EQ(back2[0].location, std::string("/m/a.mp3"));
    CHECK(write_pls(es).find("NumberOfEntries=2") != std::string::npos);
    // shuffle: a permutation, starting with the current item, the same per seed
    auto o = shuffle_order(10, 3, 42);
    CHECK_EQ(o.size(), (size_t)10);
    CHECK_EQ(o[0], 3);
    std::vector<int> sorted = o;
    std::sort(sorted.begin(), sorted.end());
    for (int i = 0; i < 10; i++) CHECK_EQ(sorted[(size_t)i], i);
    CHECK(shuffle_order(10, 3, 42) == o);
    CHECK(shuffle_order(0, -1, 1).empty());
    // stepping through an order
    std::vector<int> ord = {2, 0, 1};
    CHECK_EQ(step(ord, 2, 1, Repeat::Off), 0);
    CHECK_EQ(step(ord, 1, 1, Repeat::Off), -1);
    CHECK_EQ(step(ord, 1, 1, Repeat::All), 2);
    CHECK_EQ(step(ord, 2, -1, Repeat::All), 1);
    CHECK_EQ(step(ord, 2, -1, Repeat::Off), -1);
    CHECK_EQ(step(ord, 0, 1, Repeat::One), 0);
    CHECK_EQ(step(ord, -1, 1, Repeat::Off), 2);
    CHECK(next_repeat(Repeat::Off) == Repeat::All);
    CHECK(next_repeat(Repeat::One) == Repeat::Off);
  }
  return test_result("video");
}
