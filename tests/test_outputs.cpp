// common/displays: the [output:<name>] sections written for fl-wm
#include <string>
#include <vector>

#include "fld/ini.h"
#include "outputs.h"
#include "test.h"

using namespace fld::outputs;

int main() {
  Mode m;
  m.w = 2560;
  m.h = 1440;
  m.refresh = 143912;
  std::vector<HeadConfig> cfg = {
      {"DP-1", true, &m, 0, 0, 1, 1.25},
      {"HDMI-A-1", false, nullptr, 2048, -100, 0, 1.0},
  };
  fld::Ini wm;
  wm.set("output:DP-1", "adaptive_sync", "on"); // other keys stay
  write_wm_config(wm, cfg);
  CHECK_EQ(wm.get("output:DP-1", "mode"), std::string("2560x1440@143.912"));
  CHECK_EQ(wm.get("output:DP-1", "scale"), std::string("1.25"));
  CHECK_EQ(wm.get("output:DP-1", "position"), std::string("0,0"));
  CHECK_EQ(wm.get("output:DP-1", "transform"), std::string("90"));
  CHECK(wm.get_bool("output:DP-1", "enabled", false));
  CHECK_EQ(wm.get("output:DP-1", "adaptive_sync"), std::string("on"));
  CHECK(!wm.get_bool("output:HDMI-A-1", "enabled", true));
  CHECK(!wm.has("output:HDMI-A-1", "mode")); // no mode known: none written
  CHECK_EQ(wm.get("output:HDMI-A-1", "position"), std::string("2048,-100"));
  CHECK_EQ(wm.get("output:HDMI-A-1", "transform"), std::string("normal"));

  Mode hd;
  hd.w = 1920;
  hd.h = 1080;
  hd.refresh = 60000;
  hd.preferred = true;
  CHECK(mode_label(hd).find("1920 × 1080 @ 60.00 Hz") == 0);
  CHECK(mode_label(hd).find("★") != std::string::npos);
  CHECK(!available()); // not connected
  return test_result("outputs");
}
