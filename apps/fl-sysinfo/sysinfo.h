// System information from /proc, /sys and os-release (no FLTK dependency).
// Every reader takes the root of the file system to read ("" for the real
// one), so the tests can point it at a made-up tree.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace sysinfo {

struct Row {
  std::string key, value;
};
struct Group {
  std::string title;
  std::vector<Row> rows;
};
// one page of the program
struct Page {
  std::string id;    // "overview", "processor", ... (fl-sysinfo --page ID)
  std::string title; // "Overview"
  std::string icon;  // theme icon name
  std::vector<Group> groups;
};

// ---- the pages ----
Page overview(const std::string &root = "");
Page processor(const std::string &root = "");
Page memory(const std::string &root = "");
Page graphics(const std::string &root = "");
Page storage(const std::string &root = "");
Page network(const std::string &root = "");
Page devices(const std::string &root = "");
Page power(const std::string &root = ""); // batteries and sensors (they change)
std::vector<Page> all_pages(const std::string &root = "");
// the ids of the pages, in order
std::vector<std::string> page_ids();
// one page by id (an empty page for an unknown id)
Page page(const std::string &id, const std::string &root = "");

// The pages as plain text (Save Report, Copy All)
std::string report(const std::vector<Page> &pages);

// ---- parsers (exposed for the tests) ----
std::map<std::string, std::string> parse_os_release(const std::string &text);

struct CpuInfo {
  std::string model, vendor;
  std::string flags;   // of the first processor
  int logical = 0;     // "processor" entries
  double mhz = 0;      // of the first processor
};
CpuInfo parse_cpuinfo(const std::string &text);

// "kB" values of /proc/meminfo, in bytes (HugePages_* stay counts)
std::map<std::string, uint64_t> parse_meminfo(const std::string &text);

// "0-3,8-11" -> 8
int count_cpu_list(const std::string &list);
// "48K" / "1024K" / "8M" (sysfs cache sizes) -> bytes
uint64_t parse_cache_size(const std::string &s);
// 2400000 (kHz) -> "2.40 GHz"; 800000 -> "800 MHz"
std::string format_khz(uint64_t khz);
// 93784 s -> "1 day, 2 hours, 3 minutes"
std::string format_uptime(double seconds);

// pci.ids / usb.ids: vendor and device names
class IdDatabase {
public:
  void parse(const std::string &text);
  bool load(const std::string &path); // false when missing
  bool empty() const { return vendors_.empty(); }
  std::string vendor(unsigned v) const;
  std::string device(unsigned v, unsigned d) const;
private:
  std::map<unsigned, std::string> vendors_;
  std::map<uint32_t, std::string> devices_;
};
// the system's databases (loaded once; empty when not installed)
const IdDatabase &pci_ids();
const IdDatabase &usb_ids();
// the name of a PCI class (0x0300xx -> "VGA compatible controller")
std::string pci_class_name(unsigned cls);

// ---- the graphics APIs, from mesa-utils and vulkan-tools ----
// eglinfo -B (the first platform's section) or glxinfo -B: an "OpenGL" group
Group parse_eglinfo(const std::string &text);
Group parse_glxinfo(const std::string &text);
// vulkaninfo --summary: one group per device ("Vulkan: <name>")
std::vector<Group> parse_vulkaninfo(const std::string &text);
// "gles2" / "vulkan" / "pixman" (fl-wm's FLWM_RENDERER) -> what the user reads
std::string describe_renderer(const std::string &name);
// Runs the tools (3 s at most each) in the background; graphics() waits for
// them. Call early to have them ready when the page is shown.
void start_graphics_probe();
bool graphics_probe_ready();
void wait_graphics_probe(); // before a report

// battery health in percent: full / design (0 when unknown)
int battery_health(uint64_t full, uint64_t design);

} // namespace sysinfo
