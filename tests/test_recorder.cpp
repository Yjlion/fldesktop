// fl-recorder: formats, names, the pipeline and the meter
#include "rec_util.h"
#include "test.h"

int main() {
  CHECK(rec::formats().size() >= 3);
  const rec::Format *o = rec::format_by_id("opus");
  CHECK(o != nullptr);
  CHECK(rec::format_by_id("aac") == nullptr);
  struct tm tm = {};
  tm.tm_year = 126; tm.tm_mon = 10; tm.tm_mday = 25; tm.tm_hour = 14; tm.tm_min = 3; tm.tm_sec = 22; tm.tm_isdst = -1;
  time_t t = mktime(&tm);
  CHECK_EQ(rec::file_name(t, *o), std::string("Recording 2026-11-25 14.03.22.opus"));
  CHECK_EQ(rec::recordings_dir("/home/u/Music/"), std::string("/home/u/Music/Recordings"));
  std::string p = rec::pipeline(*o, "/tmp/a \"b\".opus");
  CHECK(p.find("autoaudiosrc ! ") == 0);
  CHECK(p.find("level interval=100000000 post-messages=true") != std::string::npos);
  CHECK(p.find("location=\"/tmp/a \\\"b\\\".opus\"") != std::string::npos);
  CHECK_EQ(rec::meter(-80), 0.0);
  CHECK_EQ(rec::meter(0), 1.0);
  CHECK(rec::meter(-30) > 0.49 && rec::meter(-30) < 0.51);
  CHECK_EQ(rec::duration(65.4), std::string("1:05"));
  CHECK_EQ(rec::duration(3723), std::string("1:02:03"));
  CHECK_EQ(rec::duration(-1), std::string("0:00"));
  CHECK_EQ(rec::renamed("Recording 1.opus", "Meeting"), std::string("Meeting.opus"));
  CHECK_EQ(rec::renamed("a.opus", " Talk.opus "), std::string("Talk.opus"));
  CHECK_EQ(rec::renamed("a.opus", "x/y"), std::string("xy.opus"));
  CHECK_EQ(rec::renamed("a.opus", "  "), std::string(""));
  CHECK_EQ(rec::renamed("a.opus", ".."), std::string(""));
  return test_result("recorder");
}
