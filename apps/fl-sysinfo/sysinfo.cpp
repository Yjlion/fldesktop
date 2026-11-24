#include "sysinfo.h"

#include <dirent.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <future>
#include <mutex>
#include <set>
#include <sstream>

#ifndef FLD_VERSION
#define FLD_VERSION "?"
#endif

namespace sysinfo {
namespace {

// ---- small helpers -------------------------------------------------------------

bool read_file(const std::string &path, std::string &out, size_t max = 4 << 20) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out.clear();
  char buf[8192];
  while (f && out.size() < max) {
    f.read(buf, sizeof buf);
    out.append(buf, (size_t)f.gcount());
  }
  return true;
}

std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

// the first line of a (sysfs) file, trimmed
std::string read_line(const std::string &path) {
  std::string t;
  if (!read_file(path, t, 65536)) return "";
  size_t nl = t.find('\n');
  return trim(nl == std::string::npos ? t : t.substr(0, nl));
}

uint64_t read_u64(const std::string &path, bool *ok = nullptr) {
  std::string s = read_line(path);
  if (ok) *ok = !s.empty() && (isdigit((unsigned char)s[0]) || s[0] == '-');
  return strtoull(s.c_str(), nullptr, 0);
}

std::vector<std::string> split(const std::string &s, char sep) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : s) {
    if (c == sep) {
      v.push_back(cur);
      cur.clear();
    } else cur += c;
  }
  v.push_back(cur);
  return v;
}

bool starts_with(const std::string &s, const std::string &p) { return s.compare(0, p.size(), p) == 0; }

// entries of a directory, sorted naturally (cpu2 before cpu10)
std::vector<std::string> list_dir(const std::string &dir) {
  std::vector<std::string> v;
  if (DIR *d = opendir(dir.c_str())) {
    while (struct dirent *e = readdir(d))
      if (e->d_name[0] != '.') v.push_back(e->d_name);
    closedir(d);
  }
  std::sort(v.begin(), v.end(), [](const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
      if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
        size_t i2 = i, j2 = j;
        while (i2 < a.size() && isdigit((unsigned char)a[i2])) i2++;
        while (j2 < b.size() && isdigit((unsigned char)b[j2])) j2++;
        unsigned long long x = strtoull(a.substr(i, i2 - i).c_str(), nullptr, 10);
        unsigned long long y = strtoull(b.substr(j, j2 - j).c_str(), nullptr, 10);
        if (x != y) return x < y;
        i = i2;
        j = j2;
      } else {
        if (a[i] != b[j]) return a[i] < b[j];
        i++;
        j++;
      }
    }
    return a.size() - i < b.size() - j;
  });
  return v;
}

bool exists(const std::string &p) { return access(p.c_str(), F_OK) == 0; }

std::string link_name(const std::string &path) {
  char buf[4096];
  ssize_t n = readlink(path.c_str(), buf, sizeof buf - 1);
  if (n <= 0) return "";
  buf[n] = 0;
  const char *b = strrchr(buf, '/');
  return b ? b + 1 : buf;
}

std::string human_size(uint64_t bytes) {
  const char *units[] = {"bytes", "KiB", "MiB", "GiB", "TiB", "PiB"};
  double v = (double)bytes;
  int u = 0;
  while (v >= 1024 && u < 5) {
    v /= 1024;
    u++;
  }
  char buf[64];
  if (u == 0) snprintf(buf, sizeof buf, "%llu bytes", (unsigned long long)bytes);
  else snprintf(buf, sizeof buf, v < 10 ? "%.1f %s" : "%.0f %s", v, units[u]);
  return buf;
}

// disks are sold in powers of ten: "500 GB" next to "465.8 GiB"
std::string disk_size(uint64_t bytes) {
  char buf[64];
  double gb = bytes / 1e9;
  if (gb >= 1000) snprintf(buf, sizeof buf, "%.1f TB", gb / 1000);
  else if (gb >= 1) snprintf(buf, sizeof buf, "%.0f GB", gb);
  else snprintf(buf, sizeof buf, "%.0f MB", bytes / 1e6);
  return std::string(buf) + " (" + human_size(bytes) + ")";
}

std::string join(const std::vector<std::string> &v, const std::string &sep) {
  std::string r;
  for (size_t i = 0; i < v.size(); i++) r += (i ? sep : "") + v[i];
  return r;
}

void add(Group &g, const std::string &k, const std::string &v) {
  if (!v.empty()) g.rows.push_back({k, v});
}

void add_group(Page &p, Group g) {
  if (!g.rows.empty()) p.groups.push_back(std::move(g));
}

std::string os_pretty(const std::string &root) {
  std::string t;
  if (!read_file(root + "/etc/os-release", t)) read_file(root + "/usr/lib/os-release", t);
  auto m = parse_os_release(t);
  std::string n = m["PRETTY_NAME"];
  if (n.empty()) n = m["NAME"] + (m["VERSION"].empty() ? "" : " " + m["VERSION"]);
  return n.empty() || n == " " ? "Linux" : n;
}

std::string dmi(const std::string &root, const char *f) {
  std::string v = read_line(root + "/sys/class/dmi/id/" + f);
  // placeholders some firmware leaves
  static const char *junk[] = {"To be filled by O.E.M.", "To Be Filled By O.E.M.", "Default string", "System Product Name",
                               "System manufacturer", "Not Applicable", "Not Specified", "None", "0x0000"};
  for (const char *j : junk)
    if (v == j) return "";
  return v;
}

struct Gpu {
  std::string card, driver, slot, name;
  unsigned vendor = 0, device = 0;
  uint64_t vram = 0;
};

std::vector<Gpu> gpus(const std::string &root) {
  std::vector<Gpu> v;
  std::string base = root + "/sys/class/drm";
  for (auto &c : list_dir(base)) {
    if (!starts_with(c, "card") || c.find('-') != std::string::npos) continue;
    Gpu g;
    g.card = c;
    std::string dev = base + "/" + c + "/device";
    g.driver = link_name(dev + "/driver");
    g.slot = link_name(dev);
    g.vendor = (unsigned)read_u64(dev + "/vendor");
    g.device = (unsigned)read_u64(dev + "/device");
    g.vram = read_u64(dev + "/mem_info_vram_total");
    std::string name = pci_ids().device(g.vendor, g.device);
    std::string vend = pci_ids().vendor(g.vendor);
    if (!name.empty()) g.name = (vend.empty() ? "" : vend + " ") + name;
    else if (g.vendor) {
      char buf[32];
      snprintf(buf, sizeof buf, "%04x:%04x", g.vendor, g.device);
      g.name = (vend.empty() ? std::string("PCI ") + buf : vend + " (" + buf + ")");
    } else g.name = g.driver.empty() ? c : g.driver;
    v.push_back(g);
  }
  return v;
}

std::string cpu_summary(const std::string &root) {
  std::string t;
  read_file(root + "/proc/cpuinfo", t);
  CpuInfo ci = parse_cpuinfo(t);
  std::string m = ci.model.empty() ? "Unknown processor" : ci.model;
  return ci.logical > 1 ? m + " × " + std::to_string(ci.logical) : m;
}

} // namespace

// ---- parsers -------------------------------------------------------------------

std::map<std::string, std::string> parse_os_release(const std::string &text) {
  std::map<std::string, std::string> m;
  for (auto &l0 : split(text, '\n')) {
    std::string l = trim(l0);
    if (l.empty() || l[0] == '#') continue;
    size_t eq = l.find('=');
    if (eq == std::string::npos) continue;
    std::string v = l.substr(eq + 1);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front()) v = v.substr(1, v.size() - 2);
    m[l.substr(0, eq)] = v;
  }
  return m;
}

CpuInfo parse_cpuinfo(const std::string &text) {
  CpuInfo c;
  for (auto &l : split(text, '\n')) {
    size_t colon = l.find(':');
    if (colon == std::string::npos) continue;
    std::string k = trim(l.substr(0, colon)), v = trim(l.substr(colon + 1));
    if (k == "processor") c.logical++;
    else if ((k == "model name" || k == "Model" || k == "cpu model") && c.model.empty()) c.model = v;
    else if ((k == "vendor_id" || k == "CPU implementer") && c.vendor.empty()) c.vendor = v;
    else if ((k == "flags" || k == "Features") && c.flags.empty()) c.flags = v;
    else if (k == "cpu MHz" && c.mhz == 0) c.mhz = atof(v.c_str());
  }
  // collapse the runs of spaces some CPUs put in their names
  std::string m;
  for (char ch : c.model)
    if (!(ch == ' ' && !m.empty() && m.back() == ' ')) m += ch;
  c.model = m;
  return c;
}

std::map<std::string, uint64_t> parse_meminfo(const std::string &text) {
  std::map<std::string, uint64_t> m;
  for (auto &l : split(text, '\n')) {
    size_t colon = l.find(':');
    if (colon == std::string::npos) continue;
    std::string k = l.substr(0, colon), v = trim(l.substr(colon + 1));
    uint64_t n = strtoull(v.c_str(), nullptr, 10);
    if (v.size() > 3 && v.compare(v.size() - 2, 2, "kB") == 0) n *= 1024;
    m[k] = n;
  }
  return m;
}

int count_cpu_list(const std::string &list) {
  int n = 0;
  for (auto &part : split(trim(list), ',')) {
    if (part.empty()) continue;
    size_t dash = part.find('-');
    if (dash == std::string::npos) n++;
    else n += atoi(part.substr(dash + 1).c_str()) - atoi(part.substr(0, dash).c_str()) + 1;
  }
  return n;
}

uint64_t parse_cache_size(const std::string &s0) {
  std::string s = trim(s0);
  uint64_t n = strtoull(s.c_str(), nullptr, 10);
  if (s.empty()) return 0;
  switch (toupper((unsigned char)s.back())) {
    case 'K': return n << 10;
    case 'M': return n << 20;
    case 'G': return n << 30;
  }
  return n;
}

std::string format_khz(uint64_t khz) {
  char buf[32];
  if (khz >= 1000000) snprintf(buf, sizeof buf, "%.2f GHz", khz / 1e6);
  else snprintf(buf, sizeof buf, "%llu MHz", (unsigned long long)(khz / 1000));
  return buf;
}

std::string format_uptime(double seconds) {
  long s = (long)seconds;
  long d = s / 86400, h = s / 3600 % 24, m = s / 60 % 60;
  std::vector<std::string> parts;
  auto unit = [&](long n, const char *one) {
    if (n) parts.push_back(std::to_string(n) + " " + one + (n == 1 ? "" : "s"));
  };
  unit(d, "day");
  unit(h, "hour");
  if (d == 0) unit(m, "minute");
  return parts.empty() ? "less than a minute" : join(parts, ", ");
}

void IdDatabase::parse(const std::string &text) {
  unsigned vendor = 0;
  bool have_vendor = false;
  std::istringstream in(text);
  std::string l;
  while (std::getline(in, l)) {
    if (l.empty() || l[0] == '#') continue;
    // the classes (pci.ids "C 03") and other lists (usb.ids "AT", "HID"...) follow the devices
    if (!isxdigit((unsigned char)l[0]) && l[0] != '\t') {
      have_vendor = false;
      if (l.size() > 1 && l[1] == ' ') break;
      continue;
    }
    if (l[0] != '\t') {
      char *end;
      vendor = (unsigned)strtoul(l.c_str(), &end, 16);
      if (end - l.c_str() != 4) { have_vendor = false; continue; }
      vendors_[vendor] = trim(end);
      have_vendor = true;
    } else if (have_vendor && l.size() > 1 && l[1] != '\t') {
      char *end;
      unsigned dev = (unsigned)strtoul(l.c_str() + 1, &end, 16);
      devices_[(uint32_t)vendor << 16 | (dev & 0xffff)] = trim(end);
    }
  }
}

bool IdDatabase::load(const std::string &path) {
  std::string t;
  if (!read_file(path, t, 64 << 20)) return false;
  parse(t);
  return true;
}

std::string IdDatabase::vendor(unsigned v) const {
  auto it = vendors_.find(v);
  return it == vendors_.end() ? "" : it->second;
}

std::string IdDatabase::device(unsigned v, unsigned d) const {
  auto it = devices_.find((uint32_t)v << 16 | (d & 0xffff));
  return it == devices_.end() ? "" : it->second;
}

const IdDatabase &pci_ids() {
  static IdDatabase db;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    for (const char *p : {"/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids", "/usr/share/pci.ids"})
      if (db.load(p)) break;
  }
  return db;
}

const IdDatabase &usb_ids() {
  static IdDatabase db;
  static bool loaded = false;
  if (!loaded) {
    loaded = true;
    for (const char *p : {"/usr/share/hwdata/usb.ids", "/usr/share/misc/usb.ids", "/var/lib/usbutils/usb.ids"})
      if (db.load(p)) break;
  }
  return db;
}

std::string pci_class_name(unsigned cls) {
  unsigned base = cls >> 16 & 0xff, sub = cls >> 8 & 0xff;
  switch (base) {
    case 0x01:
      switch (sub) {
        case 0x01: return "IDE controller";
        case 0x04: return "RAID controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
      }
      return "Storage controller";
    case 0x02: return sub == 0x80 ? "Network controller" : sub == 0x00 ? "Ethernet controller" : "Network controller";
    case 0x03: return sub == 0x00 ? "VGA compatible controller" : sub == 0x02 ? "3D controller" : "Display controller";
    case 0x04: return sub == 0x03 ? "Audio device" : "Multimedia controller";
    case 0x05: return "Memory controller";
    case 0x06:
      switch (sub) {
        case 0x00: return "Host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
      }
      return "Bridge";
    case 0x07: return "Communication controller";
    case 0x08: return "System peripheral";
    case 0x09: return "Input device controller";
    case 0x0c:
      switch (sub) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus";
        case 0x0a: return "Thunderbolt controller";
      }
      return "Serial bus controller";
    case 0x0d: return "Wireless controller";
    case 0x10: return "Encryption controller";
    case 0x11: return "Signal processing controller";
    case 0x12: return "Processing accelerator";
    case 0x13: return "Non-essential instrumentation";
  }
  return "Device";
}

int battery_health(uint64_t full, uint64_t design) {
  if (!full || !design) return 0;
  return (int)std::lround(100.0 * (double)full / (double)design);
}

// ---- pages ---------------------------------------------------------------------

Page overview(const std::string &root) {
  Page p{"overview", "Overview", "computer", {}};
  Group sys{"System", {}};
  std::string vendor = dmi(root, "sys_vendor"), product = dmi(root, "product_name"),
              version = dmi(root, "product_version");
  std::string computer = vendor + (vendor.empty() || product.empty() ? "" : " ") + product;
  if (!version.empty() && product.find(version) == std::string::npos && version.size() < 24)
    computer += " (" + version + ")";
  add(sys, "Computer", computer);
  add(sys, "Operating system", os_pretty(root));
  struct utsname u;
  if (root.empty() && uname(&u) == 0) {
    add(sys, "Kernel", std::string(u.sysname) + " " + u.release);
    add(sys, "Architecture", u.machine);
  } else {
    add(sys, "Kernel", read_line(root + "/proc/sys/kernel/osrelease"));
  }
  add(sys, "Desktop", std::string("fldesktop ") + FLD_VERSION);
  if (root.empty()) {
    const char *st = getenv("XDG_SESSION_TYPE");
    std::string session = st && *st ? st : getenv("WAYLAND_DISPLAY") ? "wayland" : getenv("DISPLAY") ? "x11" : "";
    if (session == "wayland") session = "Wayland";
    else if (session == "x11") session = "X11";
    add(sys, "Session", session);
    char host[256] = "";
    gethostname(host, sizeof host - 1);
    add(sys, "Computer name", host);
  } else add(sys, "Computer name", read_line(root + "/proc/sys/kernel/hostname"));
  std::string up = read_line(root + "/proc/uptime");
  if (!up.empty()) add(sys, "Running for", format_uptime(atof(up.c_str())));
  add_group(p, sys);

  Group hw{"Hardware", {}};
  add(hw, "Processor", cpu_summary(root));
  std::string mt;
  read_file(root + "/proc/meminfo", mt);
  auto mem = parse_meminfo(mt);
  if (mem["MemTotal"]) add(hw, "Memory", human_size(mem["MemTotal"]));
  std::vector<std::string> g;
  for (auto &gp : gpus(root)) g.push_back(gp.name);
  add(hw, "Graphics", join(g, "\n"));
  struct statvfs v;
  if (statvfs((root.empty() ? "/" : root.c_str()), &v) == 0) {
    uint64_t total = (uint64_t)v.f_blocks * v.f_frsize, avail = (uint64_t)v.f_bavail * v.f_frsize;
    add(hw, "Disk", human_size(total) + " (" + human_size(avail) + " free)");
  }
  add_group(p, hw);

  Group fw{"Firmware", {}};
  std::string bios = dmi(root, "bios_vendor");
  std::string bv = dmi(root, "bios_version"), bd = dmi(root, "bios_date");
  add(fw, "BIOS", bios + (bv.empty() ? "" : " " + bv) + (bd.empty() ? "" : ", " + bd));
  add(fw, "Mainboard", dmi(root, "board_vendor") + (dmi(root, "board_name").empty() ? "" : " " + dmi(root, "board_name")));
  add(fw, "Boot", exists(root + "/sys/firmware/efi") ? "UEFI" : (bios.empty() ? "" : "BIOS (legacy)"));
  add_group(p, fw);
  return p;
}

Page processor(const std::string &root) {
  Page p{"processor", "Processor", "cpu", {}};
  std::string t;
  read_file(root + "/proc/cpuinfo", t);
  CpuInfo ci = parse_cpuinfo(t);
  std::string cpu = root + "/sys/devices/system/cpu";

  // topology: packages, cores, threads
  std::set<std::string> packages, cores;
  std::vector<std::string> cpus;
  for (auto &c : list_dir(cpu)) {
    if (!starts_with(c, "cpu") || c.size() < 4 || !isdigit((unsigned char)c[3])) continue;
    cpus.push_back(c);
    std::string topo = cpu + "/" + c + "/topology/";
    std::string pkg = read_line(topo + "physical_package_id"), core = read_line(topo + "core_id");
    if (!pkg.empty()) packages.insert(pkg);
    if (!core.empty()) cores.insert(pkg + ":" + core);
  }
  int threads = cpus.empty() ? ci.logical : (int)cpus.size();
  std::string online = read_line(cpu + "/online");
  int on = online.empty() ? threads : count_cpu_list(online);

  Group g{"Processor", {}};
  add(g, "Name", ci.model.empty() ? "Unknown" : ci.model);
  std::string vendor = ci.vendor == "GenuineIntel" ? "Intel" : ci.vendor == "AuthenticAMD" ? "AMD" : ci.vendor;
  add(g, "Vendor", vendor);
  if (packages.size() > 1) add(g, "Sockets", std::to_string(packages.size()));
  if (!cores.empty()) add(g, "Cores", std::to_string(cores.size()));
  add(g, "Threads", std::to_string(threads) + (on != threads ? " (" + std::to_string(on) + " online)" : ""));
  std::string fl = " " + ci.flags + " ";
  if (fl.find(" vmx ") != std::string::npos) add(g, "Virtualization", "Intel VT-x");
  else if (fl.find(" svm ") != std::string::npos) add(g, "Virtualization", "AMD-V");
  if (fl.find(" hypervisor ") != std::string::npos) add(g, "Runs in", "a virtual machine");
  add_group(p, g);

  // speeds
  Group f{"Speed", {}};
  std::string fq = cpu + "/cpu0/cpufreq/";
  uint64_t mn = read_u64(fq + "cpuinfo_min_freq"), mx = read_u64(fq + "cpuinfo_max_freq");
  if (mx) add(f, "Range", format_khz(mn) + " – " + format_khz(mx));
  add(f, "Governor", read_line(fq + "scaling_governor"));
  add(f, "Driver", read_line(fq + "scaling_driver"));
  for (auto &c : cpus) {
    uint64_t cur = read_u64(cpu + "/" + c + "/cpufreq/scaling_cur_freq");
    if (cur) add(f, "CPU " + c.substr(3), format_khz(cur));
  }
  if (f.rows.empty() && ci.mhz > 0) add(f, "Current", format_khz((uint64_t)(ci.mhz * 1000)));
  add_group(p, f);

  // caches (of cpu0; instances = threads / threads sharing one)
  Group cg{"Caches", {}};
  std::string cache = cpu + "/cpu0/cache";
  for (auto &ix : list_dir(cache)) {
    if (!starts_with(ix, "index")) continue;
    std::string d = cache + "/" + ix + "/";
    std::string level = read_line(d + "level"), type = read_line(d + "type");
    uint64_t size = parse_cache_size(read_line(d + "size"));
    if (level.empty() || !size) continue;
    int shared = std::max(1, count_cpu_list(read_line(d + "shared_cpu_list")));
    int count = std::max(1, threads / shared);
    std::string name = "L" + level + (type == "Data" ? " data" : type == "Instruction" ? " instruction" : "");
    add(cg, name, human_size(size) + (count > 1 ? " × " + std::to_string(count) : ""));
  }
  add_group(p, cg);

  Group vg{"Security", {}};
  std::string vul = cpu + "/vulnerabilities";
  for (auto &v : list_dir(vul)) {
    std::string name = v;
    std::replace(name.begin(), name.end(), '_', ' ');
    add(vg, name, read_line(vul + "/" + v));
  }
  add_group(p, vg);

  Group fg{"Features", {}};
  add(fg, "Flags", ci.flags);
  add_group(p, fg);
  return p;
}

Page memory(const std::string &root) {
  Page p{"memory", "Memory", "media-flash", {}};
  std::string t;
  read_file(root + "/proc/meminfo", t);
  auto m = parse_meminfo(t);
  Group g{"Memory", {}};
  uint64_t total = m["MemTotal"], avail = m.count("MemAvailable") ? m["MemAvailable"] : m["MemFree"];
  add(g, "Total", human_size(total));
  if (total) {
    uint64_t used = total > avail ? total - avail : 0;
    char pct[32];
    snprintf(pct, sizeof pct, " (%.0f%%)", 100.0 * used / total);
    add(g, "In use", human_size(used) + pct);
  }
  add(g, "Available", human_size(avail));
  add(g, "Cached", human_size(m["Cached"] + m["Buffers"]));
  if (m["Shmem"]) add(g, "Shared", human_size(m["Shmem"]));
  add_group(p, g);

  Group s{"Swap", {}};
  add(s, "Total", m["SwapTotal"] ? human_size(m["SwapTotal"]) : "none");
  if (m["SwapTotal"]) add(s, "In use", human_size(m["SwapTotal"] - m["SwapFree"]));
  std::string sw;
  if (read_file(root + "/proc/swaps", sw)) {
    auto lines = split(sw, '\n');
    for (size_t i = 1; i < lines.size(); i++) {
      std::istringstream in(lines[i]);
      std::string name, type;
      uint64_t size = 0;
      if (!(in >> name >> type >> size)) continue;
      std::string base = name.substr(name.rfind('/') + 1);
      add(s, base, human_size(size * 1024) + ", " + (starts_with(base, "zram") ? "compressed in memory" : type));
    }
  }
  add_group(p, s);

  if (m["HugePages_Total"]) {
    Group h{"Huge pages", {}};
    add(h, "Pages", std::to_string(m["HugePages_Total"]) + " (" + std::to_string(m["HugePages_Free"]) + " free)");
    add(h, "Page size", human_size(m["Hugepagesize"]));
    add_group(p, h);
  }
  return p;
}


// ---- OpenGL and Vulkan --------------------------------------------------------

namespace {
// "key: value" lines (leading spaces allowed)
std::map<std::string, std::string> colon_values(const std::string &text, size_t from = 0, size_t to = std::string::npos) {
  std::map<std::string, std::string> m;
  std::istringstream in(text.substr(from, to == std::string::npos ? std::string::npos : to - from));
  std::string line;
  while (std::getline(in, line)) {
    size_t c = line.find(':');
    if (c == std::string::npos) continue;
    std::string k = trim(line.substr(0, c)), v = trim(line.substr(c + 1));
    if (!k.empty() && !v.empty() && !m.count(k)) m[k] = v;
  }
  return m;
}

std::string run_tool(const std::string &cmd) {
  std::string out;
  FILE *f = popen(("timeout 3 " + cmd + " 2>/dev/null").c_str(), "r");
  if (!f) return out;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < (1u << 20)) out.append(buf, n);
  pclose(f);
  return out;
}

bool on_path(const std::string &prog) {
  std::string path = getenv("PATH") ? getenv("PATH") : "/usr/bin:/bin";
  std::istringstream in(path);
  std::string d;
  while (std::getline(in, d, ':'))
    if (!d.empty() && access((d + "/" + prog).c_str(), X_OK) == 0) return true;
  return false;
}
} // namespace

Group parse_eglinfo(const std::string &text) {
  Group g{"OpenGL", {}};
  // several platforms (Wayland, GBM, device, ...): the first one with a renderer
  size_t start = 0, end = std::string::npos;
  for (;;) {
    size_t r = text.find("renderer:", start);
    if (r == std::string::npos) return g;
    size_t sec = text.rfind(" platform:", r);
    size_t sec_start = sec == std::string::npos ? 0 : text.rfind('\n', sec);
    start = sec_start == std::string::npos ? 0 : sec_start;
    size_t next = text.find(" platform:", r);
    end = next == std::string::npos ? std::string::npos : text.rfind('\n', next);
    break;
  }
  auto m = colon_values(text, start, end);
  auto pick = [&](std::initializer_list<const char *> keys) {
    for (const char *k : keys)
      if (m.count(k)) return m[k];
    return std::string();
  };
  add(g, "Renderer", pick({"OpenGL core profile renderer", "OpenGL compatibility profile renderer", "OpenGL ES profile renderer"}));
  add(g, "Vendor", pick({"OpenGL core profile vendor", "OpenGL compatibility profile vendor", "OpenGL ES profile vendor"}));
  add(g, "OpenGL", pick({"OpenGL core profile version", "OpenGL compatibility profile version"}));
  add(g, "OpenGL ES", pick({"OpenGL ES profile version"}));
  add(g, "Shading language", pick({"OpenGL core profile shading language version"}));
  add(g, "EGL", pick({"EGL version string"}) + (m.count("EGL vendor string") ? " (" + m["EGL vendor string"] + ")" : ""));
  return g;
}

Group parse_glxinfo(const std::string &text) {
  Group g{"OpenGL", {}};
  auto m = colon_values(text);
  add(g, "Renderer", m["OpenGL renderer string"]);
  add(g, "Vendor", m["OpenGL vendor string"]);
  add(g, "OpenGL", m.count("OpenGL core profile version string") ? m["OpenGL core profile version string"]
                                                                  : m["OpenGL version string"]);
  add(g, "OpenGL ES", m["OpenGL ES profile version string"]);
  add(g, "Shading language", m["OpenGL core profile shading language version string"]);
  add(g, "Direct rendering", m["direct rendering"]);
  if (m.count("Accelerated")) add(g, "Hardware accelerated", m["Accelerated"]);
  add(g, "Video memory", m["Video memory"]);
  return g;
}

std::vector<Group> parse_vulkaninfo(const std::string &text) {
  std::vector<Group> v;
  std::istringstream in(text);
  std::string line, instance;
  Group *cur = nullptr;
  std::map<std::string, std::string> dev;
  auto flush = [&]() {
    if (!cur) return;
    static const std::map<std::string, std::string> types = {
        {"PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU", "Integrated GPU"},
        {"PHYSICAL_DEVICE_TYPE_DISCRETE_GPU", "Discrete GPU"},
        {"PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU", "Virtual GPU"},
        {"PHYSICAL_DEVICE_TYPE_CPU", "Software (processor)"},
        {"PHYSICAL_DEVICE_TYPE_OTHER", "Other"}};
    cur->title = "Vulkan: " + (dev.count("deviceName") ? dev["deviceName"] : std::string("device"));
    auto t = types.find(dev["deviceType"]);
    add(*cur, "Type", t != types.end() ? t->second : dev["deviceType"]);
    add(*cur, "Vulkan", dev["apiVersion"]);
    add(*cur, "Driver", dev["driverName"]);
    add(*cur, "Driver version", dev.count("driverInfo") ? dev["driverInfo"] : dev["driverVersion"]);
    add(*cur, "Conformance", dev["conformanceVersion"]);
    if (!instance.empty()) add(*cur, "Loader (instance)", instance);
    dev.clear();
  };
  while (std::getline(in, line)) {
    std::string t = trim(line);
    if (starts_with(t, "Vulkan Instance Version:")) instance = trim(t.substr(24));
    else if (t.size() > 3 && starts_with(t, "GPU") && t.back() == ':' && isdigit((unsigned char)t[3])) {
      flush();
      v.push_back({"Vulkan", {}});
      cur = &v.back();
    } else if (cur) {
      size_t eq = t.find('=');
      if (eq != std::string::npos) dev[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
  }
  flush();
  return v;
}

std::string describe_renderer(const std::string &name) {
  if (name == "gles2") return "Graphics card (OpenGL ES)";
  if (name == "vulkan") return "Graphics card (Vulkan)";
  if (name == "pixman") return "Software (processor)";
  return name;
}

namespace {
std::once_flag g_probe_once;
std::shared_future<std::vector<Group>> g_probe;

std::vector<Group> probe_graphics() {
  std::vector<Group> out;
  bool egl = on_path("eglinfo"), glx = on_path("glxinfo");
  Group gl{"OpenGL", {}};
  if (egl && getenv("WAYLAND_DISPLAY")) gl = parse_eglinfo(run_tool("eglinfo -B"));
  if (gl.rows.empty() && glx) gl = parse_glxinfo(run_tool("glxinfo -B"));
  if (gl.rows.empty() && egl) gl = parse_eglinfo(run_tool("eglinfo -B"));
  if (gl.rows.empty() && !egl && !glx) add(gl, "Details", "Install mesa-utils to see the OpenGL renderer");
  if (!gl.rows.empty()) out.push_back(gl);
  if (on_path("vulkaninfo")) {
    auto vk = parse_vulkaninfo(run_tool("vulkaninfo --summary"));
    if (vk.empty()) vk.push_back({"Vulkan", {{"Devices", "none (no Vulkan driver)"}}});
    for (auto &g : vk) out.push_back(g);
  } else {
    out.push_back({"Vulkan", {{"Details", "Install vulkan-tools to see the Vulkan devices"}}});
  }
  return out;
}
} // namespace

void start_graphics_probe() {
  std::call_once(g_probe_once, [] { g_probe = std::async(std::launch::async, probe_graphics).share(); });
}

void wait_graphics_probe() {
  start_graphics_probe();
  g_probe.wait();
}

bool graphics_probe_ready() {
  start_graphics_probe();
  return g_probe.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

Page graphics(const std::string &root) {
  Page p{"graphics", "Graphics", "video-display", {}};
  std::string base = root + "/sys/class/drm";
  for (auto &gp : gpus(root)) {
    Group g{gp.name, {}};
    add(g, "Driver", gp.driver);
    add(g, "PCI slot", gp.slot.find(':') != std::string::npos ? gp.slot : "");
    if (gp.vram) add(g, "Video memory", human_size(gp.vram));
    // outputs: card0-HDMI-A-1
    for (auto &c : list_dir(base)) {
      if (!starts_with(c, gp.card + "-")) continue;
      std::string d = base + "/" + c + "/";
      std::string status = read_line(d + "status");
      std::string name = c.substr(gp.card.size() + 1);
      std::string mode = read_line(d + "modes");
      std::string v = status == "connected" ? "connected" + (mode.empty() ? "" : ", " + mode) : status;
      if (read_line(d + "enabled") == "enabled" && status != "connected") v += " (enabled)";
      add(g, name, v);
    }
    add_group(p, g);
  }
  if (p.groups.empty()) p.groups.push_back({"Graphics", {{"Devices", "none found"}}});
  if (root.empty()) { // the running system: the desktop's renderer, OpenGL, Vulkan
    if (const char *r = getenv("FLWM_RENDERER")) p.groups.push_back({"Desktop", {{"Drawn by", describe_renderer(r)}}});
    if (graphics_probe_ready())
      for (auto &g : g_probe.get()) p.groups.push_back(g);
    else
      p.groups.push_back({"OpenGL and Vulkan", {{"Details", "Reading…"}}});
  }
  return p;
}

Page storage(const std::string &root) {
  Page p{"storage", "Storage", "drive-harddisk", {}};
  std::string base = root + "/sys/block";
  for (auto &b : list_dir(base)) {
    if (starts_with(b, "loop") || starts_with(b, "ram")) continue;
    std::string d = base + "/" + b + "/";
    uint64_t sectors = read_u64(d + "size");
    if (!sectors) continue;
    std::string model = trim(read_line(d + "device/model")), vendor = trim(read_line(d + "device/vendor"));
    std::string kind = starts_with(b, "nvme")      ? "NVMe SSD"
                       : starts_with(b, "zram")    ? "Compressed memory (zram)"
                       : starts_with(b, "dm-")     ? "Device mapper volume"
                       : starts_with(b, "mmcblk")  ? "SD/MMC card"
                       : starts_with(b, "sr")      ? "Optical drive"
                       : read_line(d + "queue/rotational") == "1" ? "Hard disk"
                                                                  : "SSD";
    std::string title = "/dev/" + b;
    std::string name = (vendor.empty() || vendor == "ATA" ? "" : vendor + " ") + model;
    if (!name.empty()) title += " — " + name;
    Group g{title, {}};
    add(g, "Kind", kind + (read_line(d + "removable") == "1" ? ", removable" : ""));
    add(g, "Size", disk_size(sectors * 512));
    add(g, "Serial", read_line(d + "device/serial"));
    add(g, "Firmware", read_line(d + "device/firmware_rev"));
    std::vector<std::string> parts;
    for (auto &c : list_dir(base + "/" + b))
      if (starts_with(c, b) && exists(d + c + "/partition")) parts.push_back(c);
    if (!parts.empty()) add(g, "Partitions", std::to_string(parts.size()));
    add_group(p, g);
  }
  // mounted file systems
  std::string mt;
  if (read_file(root + "/proc/mounts", mt)) {
    Group g{"File systems", {}};
    std::set<std::string> seen;
    for (auto &l : split(mt, '\n')) {
      std::istringstream in(l);
      std::string dev, mp, fs;
      if (!(in >> dev >> mp >> fs) || !starts_with(dev, "/dev/") || seen.count(dev)) continue;
      seen.insert(dev);
      // \040 in mount points
      std::string m;
      for (size_t i = 0; i < mp.size(); i++) {
        if (mp[i] == '\\' && i + 3 < mp.size()) {
          m += (char)strtol(mp.substr(i + 1, 3).c_str(), nullptr, 8);
          i += 3;
        } else m += mp[i];
      }
      std::string v = fs + " on " + m;
      struct statvfs sv;
      if (root.empty() && statvfs(m.c_str(), &sv) == 0 && sv.f_blocks) {
        uint64_t total = (uint64_t)sv.f_blocks * sv.f_frsize, avail = (uint64_t)sv.f_bavail * sv.f_frsize;
        v += ", " + human_size(total) + " (" + human_size(avail) + " free)";
      }
      add(g, dev, v);
    }
    add_group(p, g);
  }
  return p;
}

Page network(const std::string &root) {
  Page p{"network", "Network", "network-wired", {}};
  // addresses (only of the running system)
  std::map<std::string, std::vector<std::string>> addrs;
  struct ifaddrs *ifa = nullptr;
  if (root.empty() && getifaddrs(&ifa) == 0) {
    for (auto *a = ifa; a; a = a->ifa_next) {
      if (!a->ifa_addr || (a->ifa_addr->sa_family != AF_INET && a->ifa_addr->sa_family != AF_INET6)) continue;
      char host[NI_MAXHOST];
      socklen_t len = a->ifa_addr->sa_family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
      if (getnameinfo(a->ifa_addr, len, host, sizeof host, nullptr, 0, NI_NUMERICHOST) == 0) {
        std::string h = host;
        size_t pct = h.find('%');
        if (pct != std::string::npos) h = h.substr(0, pct);
        addrs[a->ifa_name].push_back(h);
      }
    }
    freeifaddrs(ifa);
  }
  std::string base = root + "/sys/class/net";
  for (auto &n : list_dir(base)) {
    if (n == "lo") continue;
    std::string d = base + "/" + n + "/";
    std::string kind = exists(d + "wireless") || exists(d + "phy80211") ? "Wi-Fi"
                       : exists(d + "bridge")                          ? "Bridge"
                       : exists(d + "device")                          ? "Wired"
                                                                       : "Virtual";
    if (n.compare(0, 2, "wg") == 0 || n.compare(0, 3, "tun") == 0) kind = "VPN";
    Group g{n, {}};
    add(g, "Kind", kind);
    std::string state = read_line(d + "operstate");
    add(g, "State", state == "up" ? "connected" : state == "down" ? "disconnected" : state);
    bool ok = false;
    uint64_t speed = read_u64(d + "speed", &ok);
    if (ok && speed && speed < 1000000 && state == "up")
      add(g, "Speed", speed >= 1000 ? std::to_string(speed / 1000) + " Gbit/s" : std::to_string(speed) + " Mbit/s");
    add(g, "Hardware address", read_line(d + "address"));
    add(g, "Driver", link_name(d + "device/driver"));
    for (auto &a : addrs[n]) add(g, a.find(':') != std::string::npos ? "IPv6 address" : "IPv4 address", a);
    add_group(p, g);
  }
  return p;
}

Page devices(const std::string &root) {
  Page p{"devices", "Devices", "preferences-desktop-peripherals", {}};
  Group pci{"PCI", {}};
  std::string pb = root + "/sys/bus/pci/devices";
  for (auto &s : list_dir(pb)) {
    std::string d = pb + "/" + s + "/";
    unsigned v = (unsigned)read_u64(d + "vendor"), dv = (unsigned)read_u64(d + "device");
    unsigned cls = (unsigned)read_u64(d + "class");
    std::string vend = pci_ids().vendor(v), name = pci_ids().device(v, dv);
    char ids[32];
    snprintf(ids, sizeof ids, "%04x:%04x", v, dv);
    std::string what = pci_class_name(cls) + ": " +
                       (name.empty() ? (vend.empty() ? std::string(ids) : vend + " (" + ids + ")") : vend + " " + name);
    std::string drv = link_name(d + "driver");
    if (!drv.empty()) what += " [" + drv + "]";
    // 0000:00:02.0 -> 00:02.0
    add(pci, starts_with(s, "0000:") ? s.substr(5) : s, what);
  }
  add_group(p, pci);

  Group usb{"USB", {}};
  std::string ub = root + "/sys/bus/usb/devices";
  for (auto &s : list_dir(ub)) {
    std::string d = ub + "/" + s + "/";
    if (s.find(':') != std::string::npos || !exists(d + "idVendor")) continue; // interfaces
    unsigned v = (unsigned)strtoul(read_line(d + "idVendor").c_str(), nullptr, 16);
    unsigned dv = (unsigned)strtoul(read_line(d + "idProduct").c_str(), nullptr, 16);
    std::string man = read_line(d + "manufacturer"), prod = read_line(d + "product");
    std::string dbv = usb_ids().vendor(v), dbp = usb_ids().device(v, dv);
    std::string name = !dbp.empty() ? (dbv.empty() ? "" : dbv + " ") + dbp
                       : !prod.empty() ? (man.empty() ? "" : man + " ") + prod
                                       : dbv;
    char ids[32];
    snprintf(ids, sizeof ids, "%04x:%04x", v, dv);
    if (name.empty()) name = ids;
    std::string speed = read_line(d + "speed");
    if (!speed.empty()) name += " (" + (speed == "480" ? std::string("USB 2") : speed == "12" || speed == "1.5" ? "USB 1"
                                        : atoi(speed.c_str()) >= 5000 ? "USB 3" : speed + " Mbit/s") + ")";
    std::string bus = read_line(d + "busnum"), dev = read_line(d + "devnum");
    add(usb, bus.empty() ? s : "Bus " + bus + " device " + dev, name);
  }
  add_group(p, usb);

  Group in{"Input", {}};
  std::string ib = root + "/sys/class/input";
  std::set<std::string> names;
  for (auto &s : list_dir(ib)) {
    if (!starts_with(s, "input")) continue;
    std::string n = read_line(ib + "/" + s + "/name");
    if (!n.empty() && names.insert(n).second) in.rows.push_back({"", n});
  }
  for (size_t i = 0; i < in.rows.size(); i++) in.rows[i].key = std::to_string(i + 1);
  add_group(p, in);

  Group snd{"Sound", {}};
  std::string cards;
  if (read_file(root + "/proc/asound/cards", cards)) {
    for (auto &l : split(cards, '\n')) {
      size_t c = l.find(" - ");
      if (c != std::string::npos && l.find('[') != std::string::npos) add(snd, "Card " + trim(l.substr(0, l.find('['))), trim(l.substr(c + 3)));
    }
  }
  add_group(p, snd);
  return p;
}

Page power(const std::string &root) {
  Page p{"power", "Power & Sensors", "battery", {}};
  std::string pb = root + "/sys/class/power_supply";
  for (auto &s : list_dir(pb)) {
    std::string d = pb + "/" + s + "/";
    std::string type = read_line(d + "type");
    if (type == "Battery") {
      if (read_line(d + "scope") == "Device") continue; // a mouse's battery
      Group g{"Battery " + s, {}};
      std::string model = read_line(d + "manufacturer") + " " + read_line(d + "model_name");
      add(g, "Model", trim(model));
      add(g, "State", read_line(d + "status"));
      std::string cap = read_line(d + "capacity");
      if (!cap.empty()) add(g, "Charge", cap + "%");
      uint64_t full = read_u64(d + "energy_full"), design = read_u64(d + "energy_full_design");
      bool energy = full || design;
      if (!energy) {
        full = read_u64(d + "charge_full");
        design = read_u64(d + "charge_full_design");
      }
      int health = battery_health(full, design);
      if (health) {
        char buf[96];
        if (energy) snprintf(buf, sizeof buf, "%d%% (%.1f of %.1f Wh)", health, full / 1e6, design / 1e6);
        else snprintf(buf, sizeof buf, "%d%% (%.0f of %.0f mAh)", health, full / 1e3, design / 1e3);
        add(g, "Health", buf);
      }
      bool ok = false;
      uint64_t cycles = read_u64(d + "cycle_count", &ok);
      if (ok && cycles) add(g, "Charge cycles", std::to_string(cycles));
      add(g, "Technology", read_line(d + "technology"));
      add_group(p, g);
    } else if (type == "Mains") {
      Group g{"Power supply " + s, {{"Plugged in", read_line(d + "online") == "1" ? "yes" : "no"}}};
      add_group(p, g);
    }
  }
  // temperatures and fans
  std::string hb = root + "/sys/class/hwmon";
  for (auto &h : list_dir(hb)) {
    std::string d = hb + "/" + h + "/";
    std::string name = read_line(d + "name");
    Group g{"Sensors: " + (name.empty() ? h : name), {}};
    for (auto &f : list_dir(hb + "/" + h)) {
      bool temp = starts_with(f, "temp"), fan = starts_with(f, "fan");
      if (!(temp || fan) || f.size() < 7 || f.compare(f.size() - 6, 6, "_input") != 0) continue;
      std::string stem = f.substr(0, f.size() - 6);
      bool ok = false;
      long long v = (long long)read_u64(d + f, &ok);
      if (!ok) continue;
      std::string label = read_line(d + stem + "_label");
      if (label.empty()) label = temp ? "Temperature " + stem.substr(4) : "Fan " + stem.substr(3);
      char buf[48];
      if (temp) snprintf(buf, sizeof buf, "%.1f °C", v / 1000.0);
      else snprintf(buf, sizeof buf, "%lld rpm", v);
      add(g, label, buf);
    }
    add_group(p, g);
  }
  if (p.groups.empty()) p.groups.push_back({"Power", {{"Batteries and sensors", "none found"}}});
  return p;
}

std::vector<std::string> page_ids() {
  return {"overview", "processor", "memory", "graphics", "storage", "network", "devices", "power"};
}

Page page(const std::string &id, const std::string &root) {
  if (id == "overview") return overview(root);
  if (id == "processor") return processor(root);
  if (id == "memory") return memory(root);
  if (id == "graphics") return graphics(root);
  if (id == "storage") return storage(root);
  if (id == "network") return network(root);
  if (id == "devices") return devices(root);
  if (id == "power") return power(root);
  return Page{id, id, "", {}};
}

std::vector<Page> all_pages(const std::string &root) {
  std::vector<Page> v;
  for (auto &id : page_ids()) v.push_back(page(id, root));
  return v;
}

std::string report(const std::vector<Page> &pages) {
  std::string r;
  for (auto &p : pages) {
    r += p.title + "\n" + std::string(p.title.size(), '=') + "\n\n";
    for (auto &g : p.groups) {
      if (!g.title.empty() && g.title != p.title) r += g.title + "\n";
      size_t w = 0;
      for (auto &row : g.rows) w = std::max(w, row.key.size());
      for (auto &row : g.rows) {
        std::string v = row.value;
        // continuation lines line up with the value
        std::string pad(w + 4, ' ');
        for (size_t i = 0; (i = v.find('\n', i)) != std::string::npos; i += pad.size() + 1) v.insert(i + 1, pad);
        r += "  " + row.key + std::string(w - row.key.size() + 2, ' ') + v + "\n";
      }
      r += "\n";
    }
  }
  return r;
}

} // namespace sysinfo
