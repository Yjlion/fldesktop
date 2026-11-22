// fl-disks-helper: the root part of fl-disks, run with pkexec.
//
//   fl-disks-helper mklabel <disk> gpt|msdos
//   fl-disks-helper mkpart  <disk> <start> <end> <fs|none> <name|-> <label|->
//   fl-disks-helper delete  <disk> <n>
//   fl-disks-helper format  <disk> <n> <fs> <label|->
//   fl-disks-helper name    <disk> <n> <name|->
//   fl-disks-helper flag    <disk> <n> <flag> on|off
//   fl-disks-helper resize  <disk> <n> <new size>
//   fl-disks-helper move    <disk> <n> <new start>       (partitions without a file system)
//   fl-disks-helper image-read  <disk> <file>
//   fl-disks-helper image-write <file> <disk>
//
// <disk> is a whole disk (/dev/sdb, /dev/nvme0n1, ...), positions and sizes
// are bytes on 1 MiB boundaries, <fs> is ext4, btrfs, xfs, vfat, exfat,
// ntfs or swap. Nothing that is mounted, used as swap or held by another
// device (LUKS, LVM, RAID) is changed. Progress goes to stdout as
// "STEP <text>" and "PROGRESS <done> <total>" lines; on failure the last
// lines are the error. Image files are opened with the rights of the user who
// ran pkexec, not as root.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <linux/fs.h>
#include <vector>

#include "disks.h"
#include "fld/util.h"

using namespace disks;

namespace {

int fail(const std::string &m) {
  printf("%s\n", m.c_str());
  fflush(stdout);
  return 1;
}

void step(const std::string &what) {
  printf("STEP %s\n", what.c_str());
  fflush(stdout);
}

bool allow_loop() {
  // a switch for tests run with sudo; pkexec clears the environment
  const char *e = getenv("FLD_DISKS_ALLOW_LOOP");
  return e && !strcmp(e, "1") && !getenv("PKEXEC_UID");
}

uid_t caller_uid() {
  const char *u = getenv("PKEXEC_UID");
  if (u && *u) return (uid_t)strtoul(u, nullptr, 10);
  const char *s = getenv("SUDO_UID");
  if (s && *s) return (uid_t)strtoul(s, nullptr, 10);
  return getuid();
}

std::string sys(const std::string &name, const std::string &file) {
  std::string v;
  fld::read_file("/sys/class/block/" + name + "/" + file, v, 4096);
  return fld::trim(v);
}

std::string find_tool(const std::string &name) {
  for (const char *d : {"/usr/sbin/", "/usr/bin/", "/sbin/", "/bin/"}) {
    std::string p = d + name;
    if (access(p.c_str(), X_OK) == 0) return p;
  }
  return "";
}

// Runs a tool (argv[0] is looked up in the system directories); its output
// is the error. e2fsck exits 1 when it fixed something: that is fine.
bool run(std::vector<std::string> argv, std::string &err, const std::string &input = "") {
  std::string path = find_tool(argv[0]);
  if (path.empty()) {
    err = argv[0] + " is not installed.";
    return false;
  }
  std::string name = argv[0];
  argv[0] = path;
  std::string out;
  int rc = fld::run_capture_input(argv, input, out, true);
  if (rc == 0 || (name == "e2fsck" && rc == 1)) return true;
  err = fld::trim(out);
  if (err.empty()) err = name + " failed (exit status " + std::to_string(rc) + ").";
  else err = name + ": " + err;
  return false;
}

// ---- what is in use ----------------------------------------------------------------

// what is mounted: device numbers (maj:min) and source device names. Both,
// because btrfs mounts show an anonymous device number.
struct Mounted {
  std::set<std::string> devnums, names;
};

Mounted mounted_now() {
  Mounted m;
  std::string mi;
  fld::read_file("/proc/self/mountinfo", mi);
  for (auto &line : fld::split(mi, '\n')) {
    auto f = fld::split(line, ' ');
    if (f.size() > 2) m.devnums.insert(f[2]);
    // ... - <fstype> <source> <options>
    for (size_t i = 0; i + 2 < f.size(); i++)
      if (f[i] == "-") {
        const std::string &src = f[i + 2];
        char buf[4096];
        if (fld::starts_with(src, "/dev/") && realpath(src.c_str(), buf)) m.names.insert(fld::basename(buf));
        break;
      }
  }
  return m;
}

std::set<std::string> swap_names() {
  std::set<std::string> out;
  std::string sw;
  fld::read_file("/proc/swaps", sw);
  for (auto &line : fld::split(sw, '\n')) {
    auto f = fld::split(line, ' ');
    if (f.empty() || f[0] == "Filename") continue;
    char buf[4096];
    if (realpath(f[0].c_str(), buf)) out.insert(fld::basename(buf));
  }
  return out;
}

// why kernel block device `name` cannot be changed now, "" when it can
std::string in_use(const std::string &name) {
  Mounted mounted = mounted_now();
  std::set<std::string> swaps = swap_names();
  std::string dev = sys(name, "dev");
  if ((!dev.empty() && mounted.devnums.count(dev)) || mounted.names.count(name))
    return "/dev/" + name + " is mounted. Unmount it first.";
  if (swaps.count(name)) return "/dev/" + name + " is used as swap. Turn it off first (swapoff).";
  if (!fld::list_dir("/sys/class/block/" + name + "/holders").empty())
    return "/dev/" + name + " is used by another device (encryption, LVM or RAID).";
  return "";
}

// ---- the disk and its partitions ---------------------------------------------------

struct Target {
  std::string name, dev;  // sdb, /dev/sdb
};

bool check_disk(const std::string &arg, Target &t, std::string &err) {
  if (!fld::starts_with(arg, "/dev/") || arg.find('/', 5) != std::string::npos) {
    err = arg + " is not a disk.";
    return false;
  }
  t.name = arg.substr(5);
  t.dev = arg;
  if (!disk_name_ok(t.name, allow_loop()) || !fld::is_dir("/sys/class/block/" + t.name) ||
      fld::file_exists("/sys/class/block/" + t.name + "/partition")) {
    err = arg + " is not a disk.";
    return false;
  }
  struct stat st;
  if (lstat(arg.c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) {
    err = arg + " is not a block device.";
    return false;
  }
  if (sys(t.name, "ro") == "1") {
    err = arg + " is read-only.";
    return false;
  }
  return true;
}

// kernel names of the disk's partitions
std::vector<std::string> partitions_of(const Target &t) {
  std::vector<std::string> out;
  for (auto &n : fld::list_dir("/sys/class/block/" + t.name))
    if (fld::starts_with(n, t.name) && fld::file_exists("/sys/class/block/" + t.name + "/" + n + "/partition"))
      out.push_back(n);
  return out;
}

std::string partition_of(const Target &t, int number) {
  for (auto &n : partitions_of(t))
    if (atoi(sys(t.name + "/" + n, "partition").c_str()) == number) return n;
  return "";
}

bool disk_free(const Target &t, std::string &err) {
  err = in_use(t.name);
  for (auto &p : partitions_of(t))
    if (err.empty()) err = in_use(p);
  return err.empty();
}

bool parse_number(const std::string &s, int &n) {
  if (s.empty() || s.size() > 3) return false;
  for (char c : s)
    if (!isdigit((unsigned char)c)) return false;
  n = atoi(s.c_str());
  return n >= 1 && n <= 256;
}

bool parse_bytes(const std::string &s, uint64_t &v) {
  if (s.empty() || s.size() > 20) return false;
  for (char c : s)
    if (!isdigit((unsigned char)c)) return false;
  v = strtoull(s.c_str(), nullptr, 10);
  return true;
}

bool parted_print(const Target &t, PartedTable &pt, std::string &err) {
  std::string path = find_tool("parted");
  if (path.empty()) {
    err = "parted is not installed.";
    return false;
  }
  std::string out;
  fld::run_capture_input({path, "-m", "-s", t.dev, "unit", "B", "print", "free"}, "", out, true);
  return parse_parted(out, pt, err);
}

bool lsblk_disk(const Target &t, Disk &d, std::string &err) {
  std::vector<std::string> cmd = lsblk_command();
  cmd.push_back(t.dev);
  std::string out;
  std::string path = find_tool("lsblk");
  if (path.empty()) {
    err = "lsblk is not installed.";
    return false;
  }
  cmd[0] = path;
  if (fld::run_capture_input(cmd, "", out) != 0) {  // util-linux < 2.39
    cmd = lsblk_command(true);
    cmd.push_back(t.dev);
    cmd[0] = path;
    fld::run_capture_input(cmd, "", out);
  }
  std::vector<Disk> ds;
  if (!parse_lsblk(out, ds, true, &err) || ds.empty()) {
    if (err.empty()) err = "lsblk does not know " + t.dev;
    return false;
  }
  d = ds[0];
  return true;
}

// tells the kernel about the new table and waits for the device nodes
void reread(const Target &t) {
  std::string e;
  run({"partprobe", t.dev}, e);
  run({"udevadm", "settle", "--timeout=10"}, e);
}

bool wait_node(const std::string &name) {
  for (int i = 0; i < 50; i++) {
    struct stat st;
    if (stat(("/dev/" + name).c_str(), &st) == 0 && S_ISBLK(st.st_mode)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  return false;
}

bool tool_for(const std::string &fs, std::string &err) {
  const FsInfo *f = fs_info(fs);
  if (!f) {
    err = "Unknown file system " + fs;
    return false;
  }
  if (find_tool(f->tool).empty()) {
    err = std::string(f->tool) + " is not installed (package " + f->package + ").";
    return false;
  }
  return true;
}

// the partition type that fits a file system (parted 3.5+ "type"; older ones keep theirs)
void set_type(const Target &t, const std::string &table, int number, const std::string &fs, const std::string &flags) {
  std::string id;
  std::string f = fs_id_from_lsblk(fs);
  if (f == "vfat" && (flags.find("esp") != std::string::npos || flags.find("boot") != std::string::npos)) return;
  if (table == "gpt") {
    if (f == "swap") id = "0657fd6d-a4ab-43c4-84e5-0933c84b4f4f";
    else if (f == "vfat" || f == "exfat" || f == "ntfs") id = "ebd0a0a2-b9e5-4433-87c0-68b6b72699c7";
    else id = "0fc63daf-8483-4772-8e79-3d69d8477de4";
  } else {
    if (f == "swap") id = "0x82";
    else if (f == "vfat") id = "0x0c";
    else if (f == "exfat" || f == "ntfs") id = "0x07";
    else id = "0x83";
  }
  std::string e;
  run({"parted", "-s", t.dev, "type", std::to_string(number), id}, e);
}

bool make_fs(const std::string &part, const std::string &fs, const std::string &label, std::string &err) {
  if (!tool_for(fs, err)) return false;
  step("Erasing old file system signatures");
  if (!run({"wipefs", "-a", "/dev/" + part}, err)) return false;
  const FsInfo *f = fs_info(fs);
  step(std::string("Creating the ") + f->title + " file system");
  return run(mkfs_command(fs, "/dev/" + part, label), err);
}

// ---- commands -----------------------------------------------------------------------

int cmd_mklabel(char **argv) {
  Target t;
  std::string err, table = argv[3];
  if (!check_disk(argv[2], t, err)) return fail(err);
  if (table != "gpt" && table != "msdos") return fail("The table has to be gpt or msdos.");
  if (!disk_free(t, err)) return fail(err);
  step("Erasing old signatures");
  run({"wipefs", "-a", t.dev}, err);  // also the old table's: parted keeps some otherwise
  err.clear();
  step("Creating a new " + std::string(table == "gpt" ? "GPT" : "MBR") + " partition table");
  if (!run(parted_mklabel(t.dev, table), err)) return fail(err);
  reread(t);
  return 0;
}

int cmd_mkpart(char **argv) {
  Target t;
  std::string err, fs = argv[5], name = argv[6], label = argv[7];
  uint64_t start = 0, end = 0;
  if (!check_disk(argv[2], t, err)) return fail(err);
  if (!parse_bytes(argv[3], start) || !parse_bytes(argv[4], end) || end <= start || start % kAlign || end % kAlign)
    return fail("The start and end have to be on 1 MiB boundaries.");
  if (name == "-") name.clear();
  if (label == "-") label.clear();
  if (!valid_partition_name(name, err)) return fail(err);
  if (fs != "none" && !tool_for(fs, err)) return fail(err);
  PartedTable pt;
  Disk d;
  if (!parted_print(t, pt, err) || !lsblk_disk(t, d, err)) return fail(err);
  if (pt.table != "gpt" && pt.table != "msdos") return fail("The disk has no partition table. Create one first.");
  bool inside = false;
  for (auto &p : pt.parts)
    if (p.free && start >= p.start && end - 1 <= p.end) inside = true;
  if (!inside) return fail("There is no free space there.");
  std::string kind = "primary";
  if (pt.table == "msdos") {
    const Partition *ext = d.extended();
    const PartedPart *ep = ext ? pt.part(ext->number) : nullptr;
    if (ep && start >= ep->start && end - 1 <= ep->end) kind = "logical";
    else {
      int primaries = 0;
      for (auto &p : pt.parts)
        if (!p.free && p.number <= 4) primaries++;
      if (primaries >= 4) return fail("An MBR disk holds four primary partitions. Use GPT for more.");
    }
  }
  std::set<int> before;
  for (auto &p : pt.parts)
    if (!p.free) before.insert(p.number);
  step("Creating the partition");
  if (!run(parted_mkpart(t.dev, pt.table == "msdos" ? "dos" : "gpt", kind, name, fs == "none" ? "ext4" : fs, start, end),
           err))
    return fail(err);
  reread(t);
  PartedTable after;
  if (!parted_print(t, after, err)) return fail(err);
  int number = 0;
  for (auto &p : after.parts)
    if (!p.free && !before.count(p.number) && p.start == start) number = p.number;
  if (!number) return fail("The partition was created but cannot be found.");
  printf("PARTITION %d\n", number);
  if (fs == "none") return 0;
  std::string part = partition_of(t, number);
  if (part.empty() || !wait_node(part)) return fail("The new partition did not show up: format it once it does.");
  if (fs == "swap" || fs == "vfat" || fs == "exfat" || fs == "ntfs") set_type(t, pt.table, number, fs, "");
  if (!make_fs(part, fs, label, err)) return fail(err);
  reread(t);
  return 0;
}

// validated disk + partition number + its kernel name, not in use
bool part_args(char **argv, Target &t, int &n, std::string &part, std::string &err) {
  if (!check_disk(argv[2], t, err)) return false;
  if (!parse_number(argv[3], n)) {
    err = std::string(argv[3]) + " is not a partition number.";
    return false;
  }
  part = partition_of(t, n);
  if (part.empty()) {
    err = t.dev + " has no partition " + argv[3] + ".";
    return false;
  }
  err = in_use(part);
  return err.empty();
}

int cmd_delete(char **argv) {
  Target t;
  int n;
  std::string part, err;
  if (!part_args(argv, t, n, part, err)) return fail(err);
  Disk d;
  if (!lsblk_disk(t, d, err)) return fail(err);
  if (const Partition *p = d.partition(n))
    if (p->extended)
      for (auto &q : d.partitions)
        if (q.logical) return fail("Delete the logical partitions inside it first.");
  step("Erasing the file system signatures");
  run({"wipefs", "-a", "/dev/" + part}, err);
  err.clear();
  step("Deleting the partition");
  if (!run(parted_rm(t.dev, n), err)) return fail(err);
  reread(t);
  return 0;
}

int cmd_format(char **argv) {
  Target t;
  int n;
  std::string part, err, fs = argv[4], label = argv[5];
  if (label == "-") label.clear();
  if (!part_args(argv, t, n, part, err)) return fail(err);
  PartedTable pt;
  if (!parted_print(t, pt, err)) return fail(err);
  const PartedPart *pp = pt.part(n);
  Disk d;
  if (lsblk_disk(t, d, err))
    if (const Partition *p = d.partition(n))
      if (p->extended) return fail("An extended partition holds other partitions: it cannot be formatted.");
  err.clear();
  if (!tool_for(fs, err)) return fail(err);
  set_type(t, pt.table, n, fs, pp ? pp->flags : "");
  reread(t);
  if (!wait_node(part)) return fail("/dev/" + part + " went away.");
  if (!make_fs(part, fs, label, err)) return fail(err);
  return 0;
}

int cmd_name(char **argv) {
  Target t;
  int n;
  std::string part, err, name = argv[4];
  if (name == "-") name.clear();
  if (!check_disk(argv[2], t, err)) return fail(err);
  if (!parse_number(argv[3], n) || partition_of(t, n).empty()) return fail("No such partition.");
  if (!valid_partition_name(name, err)) return fail(err);
  PartedTable pt;
  if (!parted_print(t, pt, err)) return fail(err);
  if (pt.table != "gpt") return fail("Only GPT partitions have names.");
  if (!run(parted_name(t.dev, n, name), err)) return fail(err);
  reread(t);
  return 0;
}

int cmd_flag(char **argv) {
  Target t;
  int n;
  std::string err, flag = argv[4], state = argv[5];
  if (!check_disk(argv[2], t, err)) return fail(err);
  if (!parse_number(argv[3], n) || partition_of(t, n).empty()) return fail("No such partition.");
  if (state != "on" && state != "off") return fail("on or off");
  PartedTable pt;
  if (!parted_print(t, pt, err)) return fail(err);
  if (!valid_flag(pt.table == "msdos" ? "dos" : pt.table, flag)) return fail("Unknown flag " + flag);
  if (!run(parted_set(t.dev, n, flag, state == "on"), err)) return fail(err);
  reread(t);
  return 0;
}

int cmd_resize(char **argv) {
  Target t;
  int n;
  std::string part, err;
  uint64_t size = 0;
  if (!part_args(argv, t, n, part, err)) return fail(err);
  if (!parse_bytes(argv[4], size)) return fail("The size has to be in bytes.");
  Disk d;
  if (!lsblk_disk(t, d, err)) return fail(err);
  const Partition *p = d.partition(n);
  if (!p) return fail("No such partition.");
  if (p->extended) return fail("Extended partitions are not resized here.");
  if (size > p->size && size > max_size(d, *p)) return fail("There is not enough free space after the partition.");
  std::vector<Step> steps;
  if (!resize_plan(t.dev, n, "/dev/" + part, p->fs_type, p->start, p->size, size, steps, err)) return fail(err);
  char tmpl[] = "/run/fl-disks-XXXXXX";
  std::string mp;
  for (auto &s : steps) {
    step(s.what);
    std::vector<std::string> a = s.argv;
    if (s.kind == Step::Mount) {
      if (!mkdtemp(tmpl)) return fail("Cannot make a temporary folder.");
      mp = tmpl;
    }
    for (auto &x : a)
      if (x == "@MP") x = mp;
    if (!run(a, err, s.input)) {
      if (!mp.empty() && s.kind != Step::Unmount) {
        std::string e2;
        run({"umount", mp}, e2);
      }
      if (!mp.empty()) rmdir(mp.c_str());
      return fail(err);
    }
    if (s.kind == Step::Unmount && !mp.empty()) {
      rmdir(mp.c_str());
      mp.clear();
    }
    if (s.argv[0] == "parted") reread(t);
  }
  return 0;
}

int cmd_move(char **argv) {
  Target t;
  int n;
  std::string part, err;
  uint64_t start = 0;
  if (!part_args(argv, t, n, part, err)) return fail(err);
  if (!parse_bytes(argv[4], start) || start % kAlign) return fail("The start has to be on a 1 MiB boundary.");
  Disk d;
  PartedTable pt;
  if (!lsblk_disk(t, d, err) || !parted_print(t, pt, err)) return fail(err);
  const Partition *p = d.partition(n);
  const PartedPart *pp = pt.part(n);
  if (!p || !pp) return fail("No such partition.");
  if (!p->fs_type.empty()) return fail("Only a partition without a file system can be moved: its data would be lost.");
  if (p->extended || p->logical) return fail("Extended and logical partitions are not moved here.");
  // the new place has to be free, or this partition's own space
  uint64_t end = start + p->size;
  bool ok = false;
  for (auto &q : pt.parts) {
    uint64_t s = q.start, e = q.end + 1;
    if (!(q.free || q.number == n)) continue;
    // merge with neighbours that are free or this partition
    for (auto &r : pt.parts)
      if ((r.free || r.number == n) && r.start == e) e = r.end + 1;
    if (start >= s && end <= e) ok = true;
  }
  if (!ok) return fail("There is no free space there.");
  step("Moving the partition");
  std::string table = pt.table == "msdos" ? "dos" : "gpt";
  if (!run(parted_rm(t.dev, n), err)) return fail(err);
  if (!run(parted_mkpart(t.dev, table, "primary", pp->name, "ext4", start, end), err)) {
    std::string e2;  // put it back where it was
    run(parted_mkpart(t.dev, table, "primary", pp->name, "ext4", pp->start, pp->end + 1), e2);
    reread(t);
    return fail(err);
  }
  reread(t);
  return 0;
}

// ---- images -------------------------------------------------------------------------

// Opens a file with the rights of the calling user (effective ids only, so
// the helper gets root back for the disk).
int open_as_caller(const std::string &path, int flags, mode_t mode, std::string &err) {
  uid_t uid = caller_uid();
  struct passwd *pw = getpwuid(uid);
  if (!pw) {
    err = "Who called fl-disks-helper?";
    return -1;
  }
  if (initgroups(pw->pw_name, pw->pw_gid) != 0 || setegid(pw->pw_gid) != 0 || seteuid(uid) != 0) {
    err = std::string("Cannot take the user's rights: ") + strerror(errno);
    return -1;
  }
  int fd = open(path.c_str(), flags | O_NOFOLLOW | O_CLOEXEC, mode);
  int e = errno;
  if (seteuid(0) != 0 || setegid(0) != 0 || setgroups(0, nullptr) != 0) {
    // cannot happen with a saved uid of 0; never go on half dropped
    fprintf(stdout, "cannot get root back\n");
    _exit(1);
  }
  if (fd < 0) err = path + ": " + strerror(e);
  return fd;
}

uint64_t device_size(int fd) {
  uint64_t n = 0;
  if (ioctl(fd, BLKGETSIZE64, &n) != 0) return 0;
  return n;
}

bool copy(int in, int out, uint64_t total, bool to_disk, std::string &err) {
  const size_t chunk = 4 * kMiB;
  std::vector<char> buf(chunk);
  uint64_t done = 0, since_sync = 0;
  auto last = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  while (done < total) {
    size_t want = (size_t)std::min<uint64_t>(chunk, total - done);
    ssize_t n = read(in, buf.data(), want);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      err = std::string("Reading failed: ") + strerror(errno);
      return false;
    }
    if (n == 0) break;
    size_t off = 0;
    while (off < (size_t)n) {
      ssize_t w = write(out, buf.data() + off, (size_t)n - off);
      if (w < 0 && errno == EINTR) continue;
      if (w <= 0) {
        err = std::string("Writing failed: ") + strerror(errno);
        return false;
      }
      off += (size_t)w;
    }
    done += (uint64_t)n;
    since_sync += (uint64_t)n;
    // keep the page cache small so the progress is real and eject is quick
    if (to_disk && since_sync >= 64 * kMiB) {
      fdatasync(out);
      since_sync = 0;
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::milliseconds(250)) {
      printf("PROGRESS %llu %llu\n", (unsigned long long)done, (unsigned long long)total);
      fflush(stdout);
      last = now;
    }
  }
  step("Writing the last data");
  if (fsync(out) != 0) {
    err = std::string("Writing failed: ") + strerror(errno);
    return false;
  }
  printf("PROGRESS %llu %llu\n", (unsigned long long)done, (unsigned long long)total);
  fflush(stdout);
  return true;
}

int cmd_image_read(char **argv) {
  Target t;
  std::string err;
  if (!check_disk(argv[2], t, err)) {
    // a read-only disk can still be backed up
    if (err.find("read-only") == std::string::npos) return fail(err);
    err.clear();
  }
  int in = open(t.dev.c_str(), O_RDONLY | O_CLOEXEC);
  if (in < 0) return fail(t.dev + ": " + strerror(errno));
  uint64_t total = device_size(in);
  int out = open_as_caller(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0644, err);
  if (out < 0) return fail(err);
  step("Copying " + t.dev + " to " + argv[3]);
  bool ok = copy(in, out, total, false, err);
  close(in);
  close(out);
  return ok ? 0 : fail(err);
}

int cmd_image_write(char **argv) {
  Target t;
  std::string err;
  if (!check_disk(argv[3], t, err)) return fail(err);
  if (!disk_free(t, err)) return fail(err);
  int in = open_as_caller(argv[2], O_RDONLY, 0, err);
  if (in < 0) return fail(err);
  struct stat st;
  if (fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(in);
    return fail(std::string(argv[2]) + " is not a regular file.");
  }
  // O_EXCL on a block device: nobody else has it open for mounting
  int out = open(t.dev.c_str(), O_WRONLY | O_EXCL | O_CLOEXEC);
  if (out < 0) {
    close(in);
    return fail(t.dev + ": " + strerror(errno));
  }
  uint64_t cap = device_size(out);
  if ((uint64_t)st.st_size > cap) {
    close(in);
    close(out);
    return fail("The image (" + size_text((uint64_t)st.st_size) + ") is larger than the disk (" + size_text(cap) + ").");
  }
  step("Writing " + std::string(argv[2]) + " to " + t.dev);
  bool ok = copy(in, out, (uint64_t)st.st_size, true, err);
  close(in);
  close(out);
  if (!ok) return fail(err);
  reread(t);
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc < 2) return fail("usage: fl-disks-helper mklabel|mkpart|delete|format|name|flag|resize|move|image-read|image-write ...");
  if (geteuid() != 0) return fail("must run as root (through pkexec)");
  umask(022);
  setvbuf(stdout, nullptr, _IOLBF, 0);
  std::string c = argv[1];
  struct Cmd {
    const char *name;
    int args;
    int (*fn)(char **);
  } cmds[] = {{"mklabel", 4, cmd_mklabel}, {"mkpart", 8, cmd_mkpart},   {"delete", 4, cmd_delete},
              {"format", 6, cmd_format},   {"name", 5, cmd_name},       {"flag", 6, cmd_flag},
              {"resize", 5, cmd_resize},   {"move", 5, cmd_move},       {"image-read", 4, cmd_image_read},
              {"image-write", 4, cmd_image_write}};
  for (auto &k : cmds)
    if (c == k.name) {
      if (argc != k.args) return fail("wrong number of arguments for " + c);
      return k.fn(argv);
    }
  return fail("unknown command " + c);
}
