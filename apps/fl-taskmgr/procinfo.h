// Process and system statistics from /proc (no FLTK dependency)
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <sys/types.h>
#include <vector>

namespace procinfo {

struct Process {
  pid_t pid = 0, ppid = 0;
  uid_t uid = 0;
  std::string name;     // comm
  std::string user;     // user name (or the uid as text)
  std::string cmdline;  // arguments joined with spaces; empty for kernel threads
  std::string exe;      // /proc/pid/exe target, may be empty (no permission)
  char state = '?';     // R, S, D, Z, T, I...
  int nice = 0, priority = 0, threads = 0;
  uint64_t rss = 0;     // bytes
  uint64_t vsize = 0;   // bytes
  uint64_t cpu_ticks = 0; // utime + stime
  uint64_t start_ticks = 0;
  uint64_t read_bytes = 0, write_bytes = 0; // from io (when readable)
  double cpu = 0;       // percent of one CPU since the previous sample
  bool kernel_thread = false;
};

struct CpuTimes {
  uint64_t user = 0, nice = 0, system = 0, idle = 0, iowait = 0, irq = 0, softirq = 0, steal = 0;
  uint64_t total() const { return user + nice + system + idle + iowait + irq + softirq + steal; }
  uint64_t busy() const { return total() - idle - iowait; }
};

struct MemInfo {
  uint64_t total = 0, free = 0, available = 0, buffers = 0, cached = 0, swap_total = 0, swap_free = 0;
  uint64_t used() const { return total > available ? total - available : 0; }
  uint64_t swap_used() const { return swap_total > swap_free ? swap_total - swap_free : 0; }
};

struct NetDev {
  std::string name;
  uint64_t rx = 0, tx = 0;
};

struct DiskDev {
  std::string name;
  uint64_t read_sectors = 0, write_sectors = 0;
};

struct System {
  CpuTimes cpu;              // all CPUs
  std::vector<CpuTimes> cores;
  MemInfo mem;
  std::vector<NetDev> net;   // without lo
  std::vector<DiskDev> disks; // whole disks only (no partitions)
  double load[3] = {0, 0, 0};
  double uptime = 0;
  int procs_running = 0, procs_total = 0;
};

// Parsers (exposed for tests)
bool parse_stat_line(const std::string &line, Process &p); // /proc/pid/stat
void parse_status(const std::string &text, Process &p);    // Uid, Threads
bool parse_cpu_stat(const std::string &text, CpuTimes &all, std::vector<CpuTimes> &cores, int &running);
void parse_meminfo(const std::string &text, MemInfo &m);
std::vector<NetDev> parse_net_dev(const std::string &text);
std::vector<DiskDev> parse_diskstats(const std::string &text);

// Samples /proc periodically and computes rates between samples.
class Sampler {
public:
  explicit Sampler(const std::string &proc_root = "/proc");
  void sample();
  const std::vector<Process> &processes() const { return procs_; }
  const System &system() const { return sys_; }
  // Rates since the previous sample (0 on the first one)
  double cpu_percent() const { return cpu_pct_; }               // all CPUs, 0..100
  const std::vector<double> &core_percent() const { return core_pct_; }
  double net_rx_rate() const { return rx_rate_; }               // bytes/s
  double net_tx_rate() const { return tx_rate_; }
  double disk_read_rate() const { return rd_rate_; }            // bytes/s
  double disk_write_rate() const { return wr_rate_; }
  long clock_ticks() const { return hz_; }
  int cpu_count() const { return (int)sys_.cores.size(); }

private:
  std::string root_;
  long hz_;
  long page_size_;
  std::vector<Process> procs_;
  System sys_;
  std::map<pid_t, uint64_t> prev_ticks_;
  CpuTimes prev_cpu_;
  std::vector<CpuTimes> prev_cores_;
  uint64_t prev_rx_ = 0, prev_tx_ = 0, prev_rd_ = 0, prev_wr_ = 0;
  double prev_time_ = 0;
  bool first_ = true;
  double cpu_pct_ = 0, rx_rate_ = 0, tx_rate_ = 0, rd_rate_ = 0, wr_rate_ = 0;
  std::vector<double> core_pct_;
  std::map<uid_t, std::string> users_;
  const std::string &user_name(uid_t uid);
};

// Actions. Return 0 or an errno value.
int send_signal(pid_t pid, int sig);
int set_nice(pid_t pid, int nice);
std::string human_bytes(double b);
std::string state_name(char s);

} // namespace procinfo
