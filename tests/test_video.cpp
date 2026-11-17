#include "test.h"
#include <unistd.h>
#include "fld/util.h"
#include <cstdlib>
#include "fld/ini.h"
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
  return test_result("video");
}
