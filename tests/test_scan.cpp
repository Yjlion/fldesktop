// fl-scan: scanimage output and arguments
#include "scan_util.h"
#include "test.h"

using namespace scan;

int main() {
  auto d = parse_devices(
      "device `airscan:e0:Canon MF240' is a eSCL Canon MF240 ip=192.168.1.5\n"
      "device `epson2:libusb:001:004' is a Epson PID 08D1 flatbed scanner\n"
      "\nNo scanners were identified.\n");
  CHECK_EQ(d.size(), (size_t)2);
  CHECK_EQ(d[0].name, std::string("airscan:e0:Canon MF240"));
  CHECK_EQ(d[0].label, std::string("Canon MF240 (network)"));
  CHECK_EQ(d[1].label, std::string("Epson PID 08D1 flatbed scanner"));
  CHECK(parse_devices("No scanners were identified.").empty());

  Options o = parse_options(
      "All options specific to device `airscan:e0:Canon':\n"
      "  Standard:\n"
      "    --resolution 75|100|150|200|300|600dpi [300]\n"
      "        Sets the resolution of the scanned image.\n"
      "    --mode Color|Gray [Color]\n"
      "    --source Flatbed|ADF Front|ADF Duplex [Flatbed]\n"
      "  Geometry:\n"
      "    -l 0..215.9mm [0]\n"
      "    -t 0..297.1mm [0]\n"
      "    -x 0..215.9mm [215.9]\n"
      "    -y 0..297.1mm [297.1]\n");
  CHECK_EQ(o.resolutions.size(), (size_t)6);
  CHECK_EQ(o.resolution, 300);
  CHECK_EQ(o.modes.size(), (size_t)2);
  CHECK_EQ(o.mode, std::string("Color"));
  CHECK_EQ(o.sources.size(), (size_t)3);
  CHECK_EQ(o.sources[1], std::string("ADF Front"));
  CHECK_NEAR(o.max_w, 215.9);
  CHECK_NEAR(o.max_h, 297.1);
  Options r = parse_options("    --resolution 50..1200dpi (in steps of 1) [150]\n");
  CHECK_EQ(r.resolutions.front(), 75);
  CHECK_EQ(r.resolutions.back(), 1200);

  Request q;
  q.device = "dev";
  q.mode = "Gray";
  q.resolution = 150;
  q.output = "/tmp/a.png";
  auto a = scanimage_args(q);
  CHECK_EQ(a[0], std::string("scanimage"));
  CHECK_EQ(a[2], std::string("dev"));
  bool has_out = false, has_area = false;
  for (auto &s : a) {
    has_out |= s == "--output-file=/tmp/a.png";
    has_area |= s.compare(0, 2, "-l") == 0;
  }
  CHECK(has_out);
  CHECK(!has_area);
  q.x = 10;
  q.y = 20;
  q.w = 100;
  q.h = 50.25;
  q.batch = true;
  q.output = "/tmp/p-%d.png";
  a = scanimage_args(q);
  bool l = false, yy = false, batch = false;
  for (auto &s : a) {
    l |= s == "-l10.0";
    yy |= s == "-y50.2" || s == "-y50.3";
    batch |= s == "--batch=/tmp/p-%d.png";
  }
  CHECK(l && yy && batch);

  CHECK_EQ(mm_to_px(25.4, 300), 300);
  CHECK_NEAR(px_to_pt(300, 300), 72.0);
  CHECK_EQ(error_text("Progress: 10%\nscanimage: open of device x failed: Device busy\n"), std::string("Device busy"));
  return test_result("scan");
}
