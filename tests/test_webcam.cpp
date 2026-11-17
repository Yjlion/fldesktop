// fl-webcam helpers
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "cam_util.h"
#include "test.h"

int main() {
  using namespace cam;
  std::vector<Mode> m = {
      {640, 480, 30, "raw"}, {1920, 1080, 5, "raw"}, {1920, 1080, 30, "jpeg"},
      {1280, 720, 30, "jpeg"}, {1280, 720, 30, "raw"}, {320, 240, 30, "raw"}, {0, 0, 0, "raw"},
  };
  auto b = best_modes(m);
  CHECK_EQ(b.size(), (size_t)4);
  CHECK_EQ(b[0].w, 1920);
  CHECK_EQ(b[0].format, std::string("jpeg")); // 30 fps beats 5 fps raw
  CHECK_EQ(b[1].w, 1280);
  CHECK_EQ(b[1].format, std::string("raw"));  // equal speed: raw
  CHECK_EQ(b[3].w, 320);
  CHECK_EQ(default_mode(b), (size_t)0);
  std::vector<Mode> big = {{3840, 2160, 30, "jpeg"}, {1920, 1080, 15, "raw"}, {1280, 720, 30, "raw"}};
  CHECK_EQ(default_mode(best_modes(big)), (size_t)2); // 4K too big, 1080p too slow
  CHECK_EQ(mode_label({1280, 720, 30, "raw"}), std::string("1280×720 (30 fps)"));
  CHECK_EQ(mode_label({640, 480, 0, "raw"}), std::string("640×480"));
  CHECK_EQ(format_duration(75), std::string("01:15"));
  CHECK_EQ(format_duration(3725), std::string("1:02:05"));

  std::string dir = "/tmp/fld-test-webcam-" + std::to_string(getpid());
  CHECK(system(("mkdir -p '" + dir + "'").c_str()) == 0);
  time_t t = 1700000000;
  std::string p1 = capture_path(dir, "jpg", t);
  CHECK(p1.find(dir + "/Webcam-") == 0);
  CHECK(p1.size() > 4 && p1.substr(p1.size() - 4) == ".jpg");
  CHECK(system(("touch '" + p1 + "'").c_str()) == 0);
  std::string p2 = capture_path(dir, "jpg", t);
  CHECK(p2 != p1);
  CHECK(p2.find("-2.jpg") != std::string::npos);
  CHECK(system(("rm -rf '" + dir + "'").c_str()) == 0);
  return test_result("webcam");
}
