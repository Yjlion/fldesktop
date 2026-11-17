#include <csignal>
#include <cstring>
#include <sys/wait.h>
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>
#include <unistd.h>

#include "procinfo.h"
#include "test.h"

using namespace procinfo;

static std::string g_root;

static void put(const std::string &rel, const std::string &text) {
  std::string path = g_root + "/" + rel;
  for (size_t i = g_root.size() + 1; (i = path.find('/', i)) != std::string::npos; i++)
    mkdir(path.substr(0, i).c_str(), 0755);
  std::ofstream(path) << text;
}

static void write_system(unsigned long long busy, unsigned long long idle) {
  char buf[512];
  snprintf(buf, sizeof buf,
           "cpu  %llu 0 0 %llu 0 0 0 0 0 0\ncpu0 %llu 0 0 %llu 0 0 0 0 0 0\ncpu1 %llu 0 0 %llu 0 0 0 0 0 0\n"
           "intr 1\nprocs_running 3\n",
           busy, idle, busy / 2, idle / 2, busy / 2, idle / 2);
  put("stat", buf);
}

static void write_proc(int pid, int ppid, const char *comm, unsigned long long utime, const std::string &cmdline,
                       unsigned long flags = 0) {
  char buf[512];
  snprintf(buf, sizeof buf,
           "%d (%s) S %d %d %d 0 -1 %lu 10 0 0 0 %llu 5 0 0 20 -2 3 0 1234 409600 25 1 2 3\n", pid, comm, ppid,
           pid, pid, flags, utime);
  std::string d = std::to_string(pid);
  put(d + "/stat", buf);
  put(d + "/status", "Name:\tx\nUid:\t0\t0\t0\t0\nThreads:\t3\n");
  put(d + "/cmdline", cmdline + std::string(1, '\0'));
}

int main() {
  // ---- parsers
  Process p;
  CHECK(parse_stat_line("42 (my (odd) prog) R 1 42 42 0 -1 4194560 1 2 3 4 100 50 0 0 20 0 7 0 999 8192 3 x", p));
  CHECK_EQ(p.pid, 42);
  CHECK_EQ(p.name, std::string("my (odd) prog"));
  CHECK_EQ(p.state, 'R');
  CHECK_EQ(p.ppid, 1);
  CHECK_EQ(p.cpu_ticks, 150ULL);
  CHECK_EQ(p.nice, 0);
  CHECK_EQ(p.threads, 7);
  CHECK_EQ(p.start_ticks, 999ULL);
  CHECK_EQ(p.vsize, 8192ULL);
  CHECK_EQ(p.rss, 3ULL);
  CHECK(!p.kernel_thread);
  CHECK(!parse_stat_line("garbage", p));

  MemInfo m;
  parse_meminfo("MemTotal: 1000 kB\nMemFree: 100 kB\nMemAvailable: 400 kB\nSwapTotal: 50 kB\nSwapFree: 20 kB\n", m);
  CHECK_EQ(m.total, 1024000ULL);
  CHECK_EQ(m.used(), 600ULL * 1024);
  CHECK_EQ(m.swap_used(), 30ULL * 1024);

  auto nets = parse_net_dev("Inter-|...\n face |...\n    lo: 5 0 0 0 0 0 0 0 5 0\n  eth0: 100 1 0 0 0 0 0 0 200 2\n");
  CHECK_EQ(nets.size(), (size_t)1);
  CHECK_EQ(nets[0].name, std::string("eth0"));
  CHECK_EQ(nets[0].rx, 100ULL);
  CHECK_EQ(nets[0].tx, 200ULL);

  auto disks = parse_diskstats("   8       0 sda 10 0 80 0 5 0 40 0 0 0 0\n   8       1 sda1 1 0 8 0 1 0 8 0 0 0 0\n"
                               "   7       0 loop0 1 0 2 0 0 0 0 0 0 0 0\n 259 0 nvme0n1 1 0 16 0 1 0 24 0 0 0 0\n"
                               " 259 1 nvme0n1p1 1 0 16 0 1 0 24 0 0 0 0\n");
  CHECK_EQ(disks.size(), (size_t)2);
  CHECK_EQ(disks[0].name, std::string("sda"));
  CHECK_EQ(disks[0].write_sectors, 40ULL);
  CHECK_EQ(disks[1].name, std::string("nvme0n1"));

  CHECK_EQ(human_bytes(512), std::string("512 B"));
  CHECK_EQ(human_bytes(1536), std::string("1.5 KiB"));
  CHECK_EQ(human_bytes(300.0 * 1024 * 1024), std::string("300 MiB"));
  CHECK_EQ(state_name('Z'), std::string("Zombie"));

  // ---- a fake /proc
  char tmpl[] = "/tmp/fld-proc-XXXXXX";
  g_root = mkdtemp(tmpl);
  write_system(1000, 9000);
  put("meminfo", "MemTotal: 2048 kB\nMemAvailable: 1024 kB\n");
  put("loadavg", "0.50 0.25 0.10 1/100 1234\n");
  put("uptime", "123.45 200.00\n");
  write_proc(1, 0, "init", 100, "/sbin/init");
  write_proc(2, 0, "kthreadd", 0, "", 0x00200000);
  write_proc(50, 1, "worker", 200, std::string("worker\0--fast", 13));

  Sampler s(g_root);
  s.sample();
  CHECK_EQ(s.processes().size(), (size_t)3);
  CHECK_EQ(s.cpu_count(), 2);
  CHECK_EQ(s.cpu_percent(), 0.0);
  CHECK_NEAR(s.system().load[0], 0.5);
  CHECK_NEAR(s.system().uptime, 123.45);
  CHECK_EQ(s.system().mem.used(), 1024ULL * 1024);
  for (auto &pr : s.processes()) {
    if (pr.pid == 2) CHECK(pr.kernel_thread);
    if (pr.pid == 50) {
      CHECK_EQ(pr.cmdline, std::string("worker --fast"));
      CHECK_EQ(pr.user, std::string("root"));
      CHECK_EQ(pr.nice, -2);
      CHECK_EQ(pr.rss, 25ULL * (uint64_t)sysconf(_SC_PAGESIZE));
    }
  }

  // 1000 more ticks over two CPUs (500 per CPU); 250 busy; worker used 250
  write_system(1250, 9750);
  write_proc(50, 1, "worker", 450, "worker");
  s.sample();
  CHECK_NEAR(s.cpu_percent(), 25.0);
  CHECK_EQ(s.core_percent().size(), (size_t)2);
  CHECK_NEAR(s.core_percent()[0], 25.0);
  for (auto &pr : s.processes()) {
    if (pr.pid == 50) CHECK_NEAR(pr.cpu, 50.0); // 250 ticks of 500 per-CPU ticks
    if (pr.pid == 1) CHECK_NEAR(pr.cpu, 0.0);
  }

  // ---- signals on a real child
  pid_t child = fork();
  if (child == 0) {
    pause();
    _exit(0);
  }
  CHECK_EQ(send_signal(child, SIGSTOP), 0);
  CHECK_EQ(send_signal(child, SIGCONT), 0);
  CHECK_EQ(set_nice(child, 5), 0);
  CHECK_EQ(send_signal(child, SIGTERM), 0);
  int st;
  waitpid(child, &st, 0);
  CHECK(WIFSIGNALED(st));
  CHECK(send_signal(child, 0) != 0);

  std::string cmd = "rm -rf " + g_root;
  CHECK_EQ(system(cmd.c_str()), 0);
  return test_result("procinfo");
}
