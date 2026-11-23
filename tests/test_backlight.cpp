// fld-backlight: percentages and steps, and devices in a fake sysfs
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>

#include "backlight.h"
#include "fld/util.h"
#include "test.h"

static void mkdev(const std::string &root, const std::string &name, const std::string &type, int max, int cur) {
  std::string d = root + "/sys/class/backlight/" + name;
  fld::mkdir_p(d, 0755);
  fld::write_file(d + "/type", type + "\n");
  fld::write_file(d + "/max_brightness", std::to_string(max) + "\n");
  fld::write_file(d + "/brightness", std::to_string(cur) + "\n");
  fld::write_file(d + "/actual_brightness", std::to_string(cur) + "\n");
}

int main() {
  using namespace bl;
  CHECK_EQ(percent(0, 100), 0);
  CHECK_EQ(percent(50, 100), 50);
  CHECK_EQ(percent(937, 1874), 50);
  CHECK_EQ(percent(5, 0), 0);
  CHECK_EQ(raw_for_percent(100, 1874), 1874);
  CHECK_EQ(raw_for_percent(0, 1874), 19); // never dark: 1 %
  CHECK_EQ(raw_for_percent(1, 7), 1);
  CHECK_EQ(raw_for_percent(50, 7), 4);
  // 5 % steps on the grid
  CHECK_EQ(step(370, 1000, 1), 400);
  CHECK_EQ(step(370, 1000, -1), 350);
  CHECK_EQ(step(400, 1000, -1), 350);
  CHECK_EQ(step(1000, 1000, 1), 1000);
  CHECK_EQ(step(50, 1000, -1), 10);  // down to 1 %, not 0
  CHECK_EQ(step(10, 1000, -1), 10);
  // few raw steps: still moves
  CHECK_EQ(step(3, 7, 1), 4);
  CHECK_EQ(step(3, 7, -1), 2);
  CHECK_EQ(step(1, 7, -1), 1);

  char tmpl[] = "/tmp/fld-backlight-XXXXXX";
  std::string root = mkdtemp(tmpl);
  setenv("FLD_SYSFS_ROOT", root.c_str(), 1);
  setenv("FLD_LOGIND_BUS", "session", 1); // no logind there: the sysfs file is written
  setenv("FLD_UPOWER_BUS", "session", 1);
  int pct = -1;
  CHECK(!screen_get(pct));
  mkdev(root, "acpi_video0", "firmware", 15, 7);
  mkdev(root, "intel_backlight", "raw", 1874, 937);
  mkdev(root, "broken", "raw", 0, 0);
  std::vector<Device> devs = list();
  CHECK_EQ(devs.size(), (size_t)2); // max 0 is left out
  const Device *d = pick(devs);
  CHECK(d && d->name == "acpi_video0"); // firmware first
  CHECK(screen_get(pct));
  CHECK_EQ(pct, 47);
  // with the firmware device gone, the raw one
  std::string out;
  fld::run_capture({"rm", "-r", root + "/sys/class/backlight/acpi_video0"}, out);
  CHECK(screen_get(pct));
  CHECK_EQ(pct, 50);
  std::string err;
  CHECK(screen_step(1, pct, err));
  CHECK_EQ(pct, 55);
  std::string v;
  fld::read_file(root + "/sys/class/backlight/intel_backlight/brightness", v);
  CHECK_EQ(fld::trim(v), std::to_string(raw_for_percent(55, 1874)));
  CHECK(screen_set(20, err));
  fld::read_file(root + "/sys/class/backlight/intel_backlight/brightness", v);
  CHECK_EQ(fld::trim(v), std::to_string(raw_for_percent(20, 1874)));
  // a name that leaves sysfs is refused
  Device bad;
  bad.name = "../x";
  bad.max = 10;
  CHECK(!set(bad, 5, err));
  // no keyboard backlight without UPower
  int kv, km;
  if (!getenv("FLD_KBD_MOCK")) CHECK(!kbd_get(kv, km));
  else {
    CHECK(kbd_get(kv, km));
    CHECK_EQ(km, 3);
    CHECK(kbd_step(1, false, pct));
    CHECK(kbd_get(kv, km));
    CHECK_EQ(kv, 2);
    CHECK(kbd_step(1, true, pct));
    CHECK(kbd_step(1, true, pct)); // past the top: off
    CHECK(kbd_get(kv, km));
    CHECK_EQ(kv, 0);
  }
  fld::run_capture({"rm", "-r", root}, out);
  return test_result("backlight");
}
