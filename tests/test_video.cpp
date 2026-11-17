#include "test.h"
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
  return test_result("video");
}
