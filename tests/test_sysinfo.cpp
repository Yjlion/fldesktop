// fl-sysinfo: parsers, and the pages read from a made-up /proc and /sys tree
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include "sysinfo.h"
#include "test.h"

static std::string g_root;

static void put(const std::string &rel, const std::string &text) {
  std::string path = g_root + rel;
  for (size_t i = 1; i < path.size(); i++)
    if (path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
  std::ofstream(path) << text;
}

static const sysinfo::Row *row(const sysinfo::Page &p, const std::string &group, const std::string &key) {
  for (auto &g : p.groups)
    if (group.empty() || g.title == group)
      for (auto &r : g.rows)
        if (r.key == key) return &r;
  return nullptr;
}
static std::string val(const sysinfo::Page &p, const std::string &group, const std::string &key) {
  const sysinfo::Row *r = row(p, group, key);
  return r ? r->value : "<none>";
}

static void test_parsers() {
  auto os = sysinfo::parse_os_release("NAME=\"Arch Linux\"\n# comment\nPRETTY_NAME='Arch Linux'\nID=arch\n");
  CHECK_EQ(os["NAME"], std::string("Arch Linux"));
  CHECK_EQ(os["PRETTY_NAME"], std::string("Arch Linux"));
  CHECK_EQ(os["ID"], std::string("arch"));

  auto ci = sysinfo::parse_cpuinfo("processor\t: 0\nvendor_id\t: GenuineIntel\nmodel name\t: Intel(R) Core(TM)   i5\n"
                                   "cpu MHz\t\t: 2400.000\nflags\t\t: fpu vmx sse\n\nprocessor\t: 1\nmodel name\t: x\n");
  CHECK_EQ(ci.logical, 2);
  CHECK_EQ(ci.model, std::string("Intel(R) Core(TM) i5"));
  CHECK_EQ(ci.vendor, std::string("GenuineIntel"));
  CHECK_EQ(ci.flags, std::string("fpu vmx sse"));
  CHECK_NEAR(ci.mhz, 2400.0);

  auto mi = sysinfo::parse_meminfo("MemTotal:       16000000 kB\nHugePages_Total:       4\n");
  CHECK_EQ(mi["MemTotal"], 16000000ull * 1024);
  CHECK_EQ(mi["HugePages_Total"], 4ull);

  CHECK_EQ(sysinfo::count_cpu_list("0-3,8-11"), 8);
  CHECK_EQ(sysinfo::count_cpu_list("5"), 1);
  CHECK_EQ(sysinfo::count_cpu_list(""), 0);
  CHECK_EQ(sysinfo::parse_cache_size("48K"), 48ull << 10);
  CHECK_EQ(sysinfo::parse_cache_size("8M\n"), 8ull << 20);
  CHECK_EQ(sysinfo::format_khz(2400000), std::string("2.40 GHz"));
  CHECK_EQ(sysinfo::format_khz(800000), std::string("800 MHz"));
  CHECK_EQ(sysinfo::format_uptime(93784), std::string("1 day, 2 hours"));
  CHECK_EQ(sysinfo::format_uptime(3720), std::string("1 hour, 2 minutes"));
  CHECK_EQ(sysinfo::format_uptime(5), std::string("less than a minute"));
  CHECK_EQ(sysinfo::battery_health(45000000, 50000000), 90);
  CHECK_EQ(sysinfo::battery_health(1, 0), 0);
  CHECK_EQ(sysinfo::pci_class_name(0x030000), std::string("VGA compatible controller"));
  CHECK_EQ(sysinfo::pci_class_name(0x010802), std::string("NVMe controller"));

  sysinfo::IdDatabase db;
  db.parse("# pci.ids\n8086  Intel Corporation\n\t1234  Some GPU\n\t\t1028 0001  subsystem\n"
           "10de  NVIDIA Corporation\n\t2504  GA106\nC 00  Unclassified device\n\t00  Non-VGA\n");
  CHECK_EQ(db.vendor(0x8086), std::string("Intel Corporation"));
  CHECK_EQ(db.device(0x8086, 0x1234), std::string("Some GPU"));
  CHECK_EQ(db.device(0x10de, 0x2504), std::string("GA106"));
  CHECK_EQ(db.device(0x8086, 0x1028), std::string("")); // a subsystem line is not a device
  CHECK_EQ(db.vendor(0x0000), std::string(""));
}

static void test_pages() {
  char tmpl[] = "/tmp/fl-sysinfo-test.XXXXXX";
  g_root = mkdtemp(tmpl);
  put("/etc/os-release", "PRETTY_NAME=\"Test OS 1.0\"\n");
  put("/proc/cpuinfo", "processor\t: 0\nvendor_id\t: AuthenticAMD\nmodel name\t: AMD Test 8-Core\nflags\t\t: svm\n"
                       "processor\t: 1\nprocessor\t: 2\nprocessor\t: 3\n");
  put("/proc/meminfo", "MemTotal:        8388608 kB\nMemAvailable:    4194304 kB\nSwapTotal:       0 kB\n");
  put("/proc/uptime", "3720.5 100.0\n");
  put("/proc/sys/kernel/osrelease", "6.99.0-test\n");
  put("/sys/class/dmi/id/sys_vendor", "Test Vendor\n");
  put("/sys/class/dmi/id/product_name", "To be filled by O.E.M.\n");
  // 2 cores with 2 threads each
  for (int c = 0; c < 4; c++) {
    std::string d = "/sys/devices/system/cpu/cpu" + std::to_string(c);
    put(d + "/topology/physical_package_id", "0\n");
    put(d + "/topology/core_id", std::to_string(c / 2) + "\n");
    put(d + "/cpufreq/scaling_cur_freq", "1800000\n");
  }
  put("/sys/devices/system/cpu/online", "0-3\n");
  put("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_min_freq", "400000\n");
  put("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", "4200000\n");
  put("/sys/devices/system/cpu/cpu0/cache/index0/level", "1\n");
  put("/sys/devices/system/cpu/cpu0/cache/index0/type", "Data\n");
  put("/sys/devices/system/cpu/cpu0/cache/index0/size", "32K\n");
  put("/sys/devices/system/cpu/cpu0/cache/index0/shared_cpu_list", "0-1\n");
  put("/sys/devices/system/cpu/cpu0/cache/index3/level", "3\n");
  put("/sys/devices/system/cpu/cpu0/cache/index3/type", "Unified\n");
  put("/sys/devices/system/cpu/cpu0/cache/index3/size", "16384K\n");
  put("/sys/devices/system/cpu/cpu0/cache/index3/shared_cpu_list", "0-3\n");
  put("/sys/devices/system/cpu/vulnerabilities/spectre_v1", "Mitigation: usercopy\n");
  put("/sys/class/power_supply/BAT0/type", "Battery\n");
  put("/sys/class/power_supply/BAT0/capacity", "77\n");
  put("/sys/class/power_supply/BAT0/status", "Discharging\n");
  put("/sys/class/power_supply/BAT0/energy_full", "45000000\n");
  put("/sys/class/power_supply/BAT0/energy_full_design", "50000000\n");
  put("/sys/class/power_supply/BAT0/cycle_count", "123\n");
  put("/sys/class/power_supply/hidpp_battery_0/type", "Battery\n");
  put("/sys/class/power_supply/hidpp_battery_0/scope", "Device\n");
  put("/sys/class/power_supply/AC/type", "Mains\n");
  put("/sys/class/power_supply/AC/online", "1\n");
  put("/sys/class/hwmon/hwmon0/name", "coretemp\n");
  put("/sys/class/hwmon/hwmon0/temp1_input", "45500\n");
  put("/sys/class/hwmon/hwmon0/temp1_label", "Package id 0\n");
  put("/sys/class/hwmon/hwmon0/temp2_input", "40000\n");
  put("/sys/class/hwmon/hwmon1/name", "fan\n");
  put("/sys/class/hwmon/hwmon1/fan1_input", "1200\n");
  put("/sys/block/sda/size", "976773168\n");
  put("/sys/block/sda/queue/rotational", "1\n");
  put("/sys/block/sda/device/model", "TEST DISK\n");
  put("/sys/block/loop0/size", "100\n");
  put("/sys/class/net/eth0/operstate", "up\n");
  put("/sys/class/net/eth0/address", "00:11:22:33:44:55\n");
  put("/sys/class/net/eth0/speed", "1000\n");
  put("/sys/class/net/eth0/device/vendor", "0x8086\n");
  put("/sys/class/net/lo/operstate", "unknown\n");

  sysinfo::Page o = sysinfo::overview(g_root);
  CHECK_EQ(val(o, "System", "Operating system"), std::string("Test OS 1.0"));
  CHECK_EQ(val(o, "System", "Computer"), std::string("Test Vendor")); // the placeholder product is left out
  CHECK_EQ(val(o, "System", "Kernel"), std::string("6.99.0-test"));
  CHECK_EQ(val(o, "System", "Running for"), std::string("1 hour, 2 minutes"));
  CHECK_EQ(val(o, "Hardware", "Processor"), std::string("AMD Test 8-Core × 4"));
  CHECK_EQ(val(o, "Hardware", "Memory"), std::string("8.0 GiB"));

  sysinfo::Page p = sysinfo::processor(g_root);
  CHECK_EQ(val(p, "Processor", "Vendor"), std::string("AMD"));
  CHECK_EQ(val(p, "Processor", "Cores"), std::string("2"));
  CHECK_EQ(val(p, "Processor", "Threads"), std::string("4"));
  CHECK_EQ(val(p, "Processor", "Virtualization"), std::string("AMD-V"));
  CHECK_EQ(val(p, "Speed", "Range"), std::string("400 MHz – 4.20 GHz"));
  CHECK_EQ(val(p, "Speed", "CPU 3"), std::string("1.80 GHz"));
  CHECK_EQ(val(p, "Caches", "L1 data"), std::string("32 KiB × 2"));
  CHECK_EQ(val(p, "Caches", "L3"), std::string("16 MiB"));
  CHECK_EQ(val(p, "Security", "spectre v1"), std::string("Mitigation: usercopy"));

  sysinfo::Page m = sysinfo::memory(g_root);
  CHECK_EQ(val(m, "Memory", "In use"), std::string("4.0 GiB (50%)"));
  CHECK_EQ(val(m, "Swap", "Total"), std::string("none"));

  sysinfo::Page pw = sysinfo::power(g_root);
  CHECK_EQ(val(pw, "Battery BAT0", "Charge"), std::string("77%"));
  CHECK_EQ(val(pw, "Battery BAT0", "Health"), std::string("90% (45.0 of 50.0 Wh)"));
  CHECK_EQ(val(pw, "Battery BAT0", "Charge cycles"), std::string("123"));
  CHECK_EQ(val(pw, "Power supply AC", "Plugged in"), std::string("yes"));
  CHECK_EQ(val(pw, "Sensors: coretemp", "Package id 0"), std::string("45.5 °C"));
  CHECK_EQ(val(pw, "Sensors: coretemp", "Temperature 2"), std::string("40.0 °C"));
  CHECK_EQ(val(pw, "Sensors: fan", "Fan 1"), std::string("1200 rpm"));
  bool mouse = false;
  for (auto &g : pw.groups) mouse = mouse || g.title.find("hidpp") != std::string::npos;
  CHECK(!mouse); // a device's own battery is not the computer's

  sysinfo::Page s = sysinfo::storage(g_root);
  CHECK_EQ(s.groups.size(), (size_t)1); // the loop device is left out
  CHECK_EQ(s.groups[0].title, std::string("/dev/sda — TEST DISK"));
  CHECK_EQ(val(s, "", "Kind"), std::string("Hard disk"));
  CHECK_EQ(val(s, "", "Size"), std::string("500 GB (466 GiB)"));

  sysinfo::Page n = sysinfo::network(g_root);
  CHECK_EQ(n.groups.size(), (size_t)1); // not lo
  CHECK_EQ(val(n, "eth0", "Kind"), std::string("Wired"));
  CHECK_EQ(val(n, "eth0", "Speed"), std::string("1 Gbit/s"));
  CHECK_EQ(val(n, "eth0", "Hardware address"), std::string("00:11:22:33:44:55"));

  std::string rep = sysinfo::report({o, pw});
  CHECK(rep.find("Overview\n========\n") != std::string::npos);
  CHECK(rep.find("  Operating system  Test OS 1.0\n") != std::string::npos);
  CHECK_EQ(sysinfo::page("nothing", g_root).groups.size(), (size_t)0);
  CHECK_EQ(sysinfo::page_ids().size(), (size_t)8);
  std::string cmd = "rm -rf '" + g_root + "'";
  if (system(cmd.c_str()) != 0) fprintf(stderr, "could not remove %s\n", g_root.c_str());
}

int main() {
  test_parsers();
  test_pages();
  return test_result("sysinfo");
}
