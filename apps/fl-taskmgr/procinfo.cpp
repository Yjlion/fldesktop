#include "procinfo.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <pwd.h>
#include <signal.h>
#include <sstream>
#include <sys/resource.h>
#include <unistd.h>

namespace procinfo {

static bool read_text(const std::string &path, std::string &out, size_t max = 1 << 20) {
  FILE *f = fopen(path.c_str(), "re");
  if (!f) return false;
  out.clear();
  char buf[8192];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0 && out.size() < max) out.append(buf, n);
  fclose(f);
  return true;
}

static uint64_t to_u64(const std::string &s) { return strtoull(s.c_str(), nullptr, 10); }

bool parse_stat_line(const std::string &line, Process &p) {
  size_t open = line.find('('), close = line.rfind(')');
  if (open == std::string::npos || close == std::string::npos || close < open) return false;
  p.pid = (pid_t)atol(line.c_str());
  p.name = line.substr(open + 1, close - open - 1);
  std::istringstream is(line.substr(close + 2));
  std::vector<std::string> f; // f[0] = field 3 (state)
  std::string w;
  while (is >> w) f.push_back(w);
  if (f.size() < 22) return false;
  auto field = [&](int n) -> const std::string & { return f[(size_t)(n - 3)]; };
  p.state = field(3)[0];
  p.ppid = (pid_t)atol(field(4).c_str());
  unsigned long flags = strtoul(field(9).c_str(), nullptr, 10);
  p.kernel_thread = (flags & 0x00200000) != 0; // PF_KTHREAD
  p.cpu_ticks = to_u64(field(14)) + to_u64(field(15));
  p.priority = atoi(field(18).c_str());
  p.nice = atoi(field(19).c_str());
  p.threads = atoi(field(20).c_str());
  p.start_ticks = to_u64(field(22));
  p.vsize = to_u64(field(23));
  p.rss = to_u64(field(24)); // pages, scaled by the caller
  return true;
}

void parse_status(const std::string &text, Process &p) {
  std::istringstream is(text);
  std::string line;
  while (std::getline(is, line)) {
    if (line.compare(0, 4, "Uid:") == 0) p.uid = (uid_t)strtoul(line.c_str() + 4, nullptr, 10);
    else if (line.compare(0, 8, "Threads:") == 0) p.threads = atoi(line.c_str() + 8);
  }
}

static CpuTimes parse_cpu_fields(const char *s) {
  CpuTimes c;
  unsigned long long v[8] = {0};
  sscanf(s, "%llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
  c.user = v[0]; c.nice = v[1]; c.system = v[2]; c.idle = v[3];
  c.iowait = v[4]; c.irq = v[5]; c.softirq = v[6]; c.steal = v[7];
  return c;
}

bool parse_cpu_stat(const std::string &text, CpuTimes &all, std::vector<CpuTimes> &cores, int &running) {
  std::istringstream is(text);
  std::string line;
  bool ok = false;
  cores.clear();
  while (std::getline(is, line)) {
    if (line.compare(0, 4, "cpu ") == 0) {
      all = parse_cpu_fields(line.c_str() + 4);
      ok = true;
    } else if (line.compare(0, 3, "cpu") == 0 && line.size() > 3 && isdigit((unsigned char)line[3])) {
      const char *sp = strchr(line.c_str(), ' ');
      if (sp) cores.push_back(parse_cpu_fields(sp + 1));
    } else if (line.compare(0, 14, "procs_running ") == 0) {
      running = atoi(line.c_str() + 14);
    }
  }
  return ok;
}

void parse_meminfo(const std::string &text, MemInfo &m) {
  std::istringstream is(text);
  std::string line;
  bool have_avail = false;
  while (std::getline(is, line)) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = line.substr(0, colon);
    uint64_t v = strtoull(line.c_str() + colon + 1, nullptr, 10) * 1024; // kB
    if (key == "MemTotal") m.total = v;
    else if (key == "MemFree") m.free = v;
    else if (key == "MemAvailable") { m.available = v; have_avail = true; }
    else if (key == "Buffers") m.buffers = v;
    else if (key == "Cached") m.cached = v;
    else if (key == "SwapTotal") m.swap_total = v;
    else if (key == "SwapFree") m.swap_free = v;
  }
  if (!have_avail) m.available = m.free + m.buffers + m.cached;
}

std::vector<NetDev> parse_net_dev(const std::string &text) {
  std::vector<NetDev> r;
  std::istringstream is(text);
  std::string line;
  while (std::getline(is, line)) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    NetDev d;
    size_t a = line.find_first_not_of(' ');
    d.name = line.substr(a, colon - a);
    if (d.name == "lo") continue;
    std::istringstream f(line.substr(colon + 1));
    uint64_t v[9] = {0};
    for (int i = 0; i < 9 && (f >> v[i]); i++) {}
    d.rx = v[0];
    d.tx = v[8];
    r.push_back(d);
  }
  return r;
}

static bool is_partition(const std::string &n, const std::vector<DiskDev> &disks) {
  for (auto &d : disks)
    if (n.size() > d.name.size() && n.compare(0, d.name.size(), d.name) == 0) return true;
  return false;
}

std::vector<DiskDev> parse_diskstats(const std::string &text) {
  std::vector<DiskDev> r;
  std::istringstream is(text);
  std::string line;
  while (std::getline(is, line)) {
    std::istringstream f(line);
    unsigned major, minor;
    std::string name;
    uint64_t rd_ios, rd_merged, rd_sectors, rd_ticks, wr_ios, wr_merged, wr_sectors;
    if (!(f >> major >> minor >> name >> rd_ios >> rd_merged >> rd_sectors >> rd_ticks >> wr_ios >> wr_merged >>
          wr_sectors))
      continue;
    if (name.compare(0, 4, "loop") == 0 || name.compare(0, 3, "ram") == 0 || name.compare(0, 4, "zram") == 0 ||
        name.compare(0, 2, "dm") == 0 || name.compare(0, 2, "md") == 0 || name.compare(0, 2, "sr") == 0)
      continue;
    if (is_partition(name, r)) continue;
    r.push_back({name, rd_sectors, wr_sectors});
  }
  return r;
}

static double now_seconds() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}

Sampler::Sampler(const std::string &root) : root_(root) {
  hz_ = sysconf(_SC_CLK_TCK);
  if (hz_ <= 0) hz_ = 100;
  page_size_ = sysconf(_SC_PAGESIZE);
  if (page_size_ <= 0) page_size_ = 4096;
}

Sampler::~Sampler() {
  for (auto &k : known_)
    if (k.second.stat_fd >= 0) close(k.second.stat_fd);
}

// stays well below the usual limit of 1024 open files
static const size_t kMaxOpenStat = 600;

static bool pread_text(int fd, std::string &out) {
  char buf[1024];
  ssize_t n = pread(fd, buf, sizeof buf, 0);
  if (n <= 0) return false;
  out.assign(buf, (size_t)n);
  return true;
}

bool Sampler::read_process(const std::string &dir, pid_t pid, Process &p, std::string &text) {
  fresh_ = false;
  auto it = known_.find(pid);
  if (it != known_.end()) {
    Known &k = it->second;
    bool ok = k.stat_fd >= 0 ? pread_text(k.stat_fd, text) : read_text(dir + "/stat", text);
    if (ok && parse_stat_line(text, p) && p.start_ticks == k.start_ticks) {
      p.uid = k.uid;
      p.name = k.name;
      p.user = k.user;
      p.cmdline = k.cmdline;
      p.exe = k.exe;
      p.kernel_thread = k.kernel_thread;
      k.seen = true;
      return true;
    }
    // gone, or the pid belongs to another process now
    if (k.stat_fd >= 0) close(k.stat_fd);
    known_.erase(it);
    p = Process();
  }
  fresh_ = true;
  int fd = -1;
  if (known_.size() < kMaxOpenStat) {
    fd = open((dir + "/stat").c_str(), O_RDONLY | O_CLOEXEC);
    opened_++;
    if (fd < 0 || !pread_text(fd, text)) {
      if (fd >= 0) close(fd);
      return false;
    }
  } else if (!read_text(dir + "/stat", text)) {
    return false;
  }
  if (!parse_stat_line(text, p)) {
    if (fd >= 0) close(fd);
    return false;
  }
  if (read_text(dir + "/status", text)) parse_status(text, p);
  p.user = user_name(p.uid);
  if (read_text(dir + "/cmdline", text, 64 << 10)) {
    while (!text.empty() && text.back() == '\0') text.pop_back();
    for (auto &c : text)
      if (c == '\0') c = ' ';
    p.cmdline = text;
    // comm is cut to 15 characters: use the program name from argv[0]
    if (p.name.size() == 15) {
      std::string a0 = text.substr(0, text.find(' '));
      a0 = a0.substr(a0.rfind('/') == std::string::npos ? 0 : a0.rfind('/') + 1);
      if (a0.size() > 15 && a0.compare(0, 15, p.name) == 0) p.name = a0;
    }
  }
  if (p.cmdline.empty() && (p.ppid == 2 || p.pid == 2)) p.kernel_thread = true;
  char link[4096];
  ssize_t n = readlink((dir + "/exe").c_str(), link, sizeof link - 1);
  if (n > 0) p.exe.assign(link, (size_t)n);
  opened_ += 3;
  Known &k = known_[pid];
  k.stat_fd = fd;
  k.start_ticks = p.start_ticks;
  k.uid = p.uid;
  k.name = p.name;
  k.user = p.user;
  k.cmdline = p.cmdline;
  k.exe = p.exe;
  k.kernel_thread = p.kernel_thread;
  k.seen = true;
  return true;
}

const std::string &Sampler::user_name(uid_t uid) {
  auto it = users_.find(uid);
  if (it != users_.end()) return it->second;
  struct passwd *pw = getpwuid(uid);
  return users_[uid] = pw ? pw->pw_name : std::to_string(uid);
}

void Sampler::sample(bool processes) {
  opened_ = 0;
  double t = now_seconds();
  double dt = first_ ? 0 : t - prev_time_;
  // ---- system
  std::string text;
  System s;
  if (read_text(root_ + "/stat", text)) parse_cpu_stat(text, s.cpu, s.cores, s.procs_running);
  if (read_text(root_ + "/meminfo", text)) parse_meminfo(text, s.mem);
  if (read_text(root_ + "/net/dev", text)) s.net = parse_net_dev(text);
  if (read_text(root_ + "/diskstats", text)) s.disks = parse_diskstats(text);
  if (read_text(root_ + "/loadavg", text)) sscanf(text.c_str(), "%lf %lf %lf", &s.load[0], &s.load[1], &s.load[2]);
  if (read_text(root_ + "/uptime", text)) s.uptime = atof(text.c_str());

  uint64_t dtotal = first_ ? 0 : s.cpu.total() - prev_cpu_.total();
  uint64_t dbusy = first_ ? 0 : s.cpu.busy() - prev_cpu_.busy();
  cpu_pct_ = dtotal ? 100.0 * (double)dbusy / (double)dtotal : 0;
  core_pct_.assign(s.cores.size(), 0);
  if (!first_ && prev_cores_.size() == s.cores.size())
    for (size_t i = 0; i < s.cores.size(); i++) {
      uint64_t tt = s.cores[i].total() - prev_cores_[i].total(), bb = s.cores[i].busy() - prev_cores_[i].busy();
      core_pct_[i] = tt ? 100.0 * (double)bb / (double)tt : 0;
    }
  uint64_t rx = 0, tx = 0, rd = 0, wr = 0;
  for (auto &n : s.net) { rx += n.rx; tx += n.tx; }
  for (auto &d : s.disks) { rd += d.read_sectors * 512; wr += d.write_sectors * 512; }
  if (dt > 0) {
    rx_rate_ = rx >= prev_rx_ ? (double)(rx - prev_rx_) / dt : 0;
    tx_rate_ = tx >= prev_tx_ ? (double)(tx - prev_tx_) / dt : 0;
    rd_rate_ = rd >= prev_rd_ ? (double)(rd - prev_rd_) / dt : 0;
    wr_rate_ = wr >= prev_wr_ ? (double)(wr - prev_wr_) / dt : 0;
  }
  prev_rx_ = rx; prev_tx_ = tx; prev_rd_ = rd; prev_wr_ = wr;

  // ---- processes
  if (!processes) {
    // the list is not shown: keep the last one, and the count
    s.procs_total = (int)procs_.size();
    prev_cpu_ = s.cpu;
    prev_cores_ = s.cores;
    sys_ = std::move(s);
    prev_time_ = t;
    first_ = false;
    return;
  }
  std::vector<Process> procs;
  std::map<pid_t, uint64_t> ticks;
  int ncores = std::max<int>(1, (int)s.cores.size());
  // ticks of one CPU during the interval
  // (since the last sample of the processes, which may be older than dtotal)
  uint64_t dproc = have_proc_total_ && s.cpu.total() >= proc_total_ ? s.cpu.total() - proc_total_ : 0;
  double per_cpu_ticks = dproc ? (double)dproc / ncores : 0;
  proc_total_ = s.cpu.total();
  have_proc_total_ = true;
  for (auto &k : known_) k.second.seen = false;
  DIR *d = opendir(root_.c_str());
  if (d) {
    while (dirent *e = readdir(d)) {
      if (!isdigit((unsigned char)e->d_name[0])) continue;
      std::string dir = root_ + "/" + e->d_name;
      Process p;
      if (!read_process(dir, (pid_t)atoi(e->d_name), p, text)) continue;
      p.rss *= (uint64_t)page_size_;
      if (read_io_ && read_text(dir + "/io", text)) {
        size_t a = text.find("read_bytes:"), b = text.find("\nwrite_bytes:");
        if (a != std::string::npos) p.read_bytes = strtoull(text.c_str() + a + 11, nullptr, 10);
        if (b != std::string::npos) p.write_bytes = strtoull(text.c_str() + b + 13, nullptr, 10);
      }
      auto it = prev_ticks_.find(p.pid);
      if (fresh_) it = prev_ticks_.end(); // a new process, maybe on an old pid
      if (it != prev_ticks_.end() && per_cpu_ticks > 0 && p.cpu_ticks >= it->second)
        p.cpu = 100.0 * (double)(p.cpu_ticks - it->second) / per_cpu_ticks;
      ticks[p.pid] = p.cpu_ticks;
      procs.push_back(std::move(p));
    }
    closedir(d);
  }
  // processes that ended
  for (auto it = known_.begin(); it != known_.end();) {
    if (it->second.seen) {
      ++it;
      continue;
    }
    if (it->second.stat_fd >= 0) close(it->second.stat_fd);
    it = known_.erase(it);
  }
  s.procs_total = (int)procs.size();
  procs_.swap(procs);
  prev_ticks_.swap(ticks);
  prev_cpu_ = s.cpu;
  prev_cores_ = s.cores;
  sys_ = std::move(s);
  prev_time_ = t;
  first_ = false;
}

int send_signal(pid_t pid, int sig) { return kill(pid, sig) == 0 ? 0 : errno; }

int set_nice(pid_t pid, int nice) {
  return setpriority(PRIO_PROCESS, (id_t)pid, nice) == 0 ? 0 : errno;
}

std::string uptime_text(double seconds) {
  long m = (long)(seconds / 60);
  long d = m / 1440, h = (m / 60) % 24, mm = m % 60;
  char b[64];
  if (d) snprintf(b, sizeof b, "%ld d %ld h", d, h);
  else if (h) snprintf(b, sizeof b, "%ld h %ld min", h, mm);
  else snprintf(b, sizeof b, "%ld min", mm);
  return b;
}

bool read_process_io(pid_t pid, uint64_t &rd, uint64_t &wr, const std::string &root) {
  std::string text;
  if (!read_text(root + "/" + std::to_string(pid) + "/io", text)) return false;
  size_t a = text.find("read_bytes:"), b = text.find("\nwrite_bytes:");
  rd = a != std::string::npos ? strtoull(text.c_str() + a + 11, nullptr, 10) : 0;
  wr = b != std::string::npos ? strtoull(text.c_str() + b + 13, nullptr, 10) : 0;
  return true;
}

std::string human_bytes(double b) {
  const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  int u = 0;
  while (b >= 1024 && u < 4) { b /= 1024; u++; }
  char buf[32];
  if (u == 0) snprintf(buf, sizeof buf, "%.0f %s", b, units[u]);
  else snprintf(buf, sizeof buf, b < 10 ? "%.1f %s" : "%.0f %s", b, units[u]);
  return buf;
}

std::string state_name(char s) {
  switch (s) {
    case 'R': return "Running";
    case 'S': return "Sleeping";
    case 'D': return "Disk sleep";
    case 'Z': return "Zombie";
    case 'T': return "Stopped";
    case 't': return "Traced";
    case 'I': return "Idle";
    case 'X': return "Dead";
    default: return std::string(1, s);
  }
}

} // namespace procinfo
