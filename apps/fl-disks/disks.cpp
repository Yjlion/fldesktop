#include "disks.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "fld/json.h"
#include "fld/util.h"

namespace disks {

using fld::Json;

// ---- model --------------------------------------------------------------------

std::string Partition::title() const {
  std::string n = fld::basename(device);
  if (!label.empty()) return n + " · " + label;
  if (!name.empty()) return n + " · " + name;
  return n;
}

const Partition *Disk::partition(int number) const {
  for (auto &p : partitions)
    if (p.number == number) return &p;
  return nullptr;
}

const Partition *Disk::extended() const {
  for (auto &p : partitions)
    if (p.extended) return &p;
  return nullptr;
}

bool Disk::any_mounted() const {
  if (!mount_points.empty()) return true;
  for (auto &p : partitions)
    if (p.mounted()) return true;
  return false;
}

std::string Disk::title() const { return model.empty() ? name : model; }

std::string Disk::table_name() const {
  if (table == "gpt") return "GPT";
  if (table == "dos") return "MBR (DOS)";
  if (table.empty()) return "None";
  return table;
}

uint64_t Disk::usable_start() const { return kAlign; }

uint64_t Disk::usable_end() const {
  // GPT keeps a backup header and partition entries (33 sectors) at the end
  uint64_t reserve = table == "gpt" ? 33ull * (uint64_t)logical_sector : 0;
  return size > reserve ? align_down(size - reserve) : 0;
}

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
uint64_t align_down(uint64_t v, uint64_t a) { return v / a * a; }

// ---- lsblk ----------------------------------------------------------------------

const char *kLsblkColumns =
    "NAME,PATH,TYPE,SIZE,START,PHY-SEC,LOG-SEC,MODEL,SERIAL,TRAN,RM,RO,PTTYPE,PARTN,PARTTYPE,PARTTYPENAME,"
    "PARTLABEL,PARTFLAGS,FSTYPE,LABEL,UUID,MOUNTPOINTS,FSUSED,FSSIZE";

std::vector<std::string> lsblk_command(bool legacy) {
  std::string cols = kLsblkColumns;
  if (legacy) {
    size_t i = cols.find("PARTN,");
    if (i != std::string::npos) cols.erase(i, 6);
  }
  return {"lsblk", "-J", "-b", "-o", cols};
}

namespace {

std::string jstr(const Json &j, const char *k) {
  const Json &v = j[k];
  return v.is_string() ? fld::trim(v.as_string()) : "";
}

// numbers may come as numbers or (older lsblk) as strings
int64_t jnum(const Json &j, const char *k, int64_t def = -1) {
  const Json &v = j[k];
  if (v.type() == Json::Number) return (int64_t)v.as_number();
  if (v.is_string() && !v.as_string().empty()) return strtoll(v.as_string().c_str(), nullptr, 10);
  return def;
}

bool jbool(const Json &j, const char *k) {
  const Json &v = j[k];
  if (v.type() == Json::Bool) return v.as_bool();
  if (v.is_string()) return v.as_string() == "1" || v.as_string() == "true";
  if (v.type() == Json::Number) return v.as_number() != 0;
  return false;
}

std::vector<std::string> jmounts(const Json &j) {
  std::vector<std::string> out;
  const Json &v = j["mountpoints"];
  if (v.is_array()) {
    for (auto &m : v.items())
      if (m.is_string() && !m.as_string().empty()) out.push_back(m.as_string());
  } else if (j["mountpoint"].is_string() && !j["mountpoint"].as_string().empty()) {
    out.push_back(j["mountpoint"].as_string());
  }
  return out;
}

bool is_extended_type(const std::string &t) {
  std::string l = fld::lower(t);
  return l == "0x5" || l == "0xf" || l == "0x85" || l == "0x05" || l == "0x0f";
}

}  // namespace

bool parse_lsblk(const std::string &json, std::vector<Disk> &out, bool with_loop, std::string *err) {
  out.clear();
  Json root;
  if (!Json::parse(json, root, err)) return false;
  for (auto &b : root["blockdevices"].items()) {
    std::string type = jstr(b, "type");
    std::string name = jstr(b, "name");
    if (type != "disk" && !(with_loop && type == "loop")) continue;
    if (!disk_name_ok(name, with_loop)) continue;
    Disk d;
    d.name = name;
    d.device = jstr(b, "path");
    if (d.device.empty()) d.device = "/dev/" + name;
    d.model = jstr(b, "model");
    d.serial = jstr(b, "serial");
    d.transport = jstr(b, "tran");
    d.table = jstr(b, "pttype");
    d.size = (uint64_t)std::max<int64_t>(0, jnum(b, "size", 0));
    d.logical_sector = (int)jnum(b, "log-sec", 512);
    d.physical_sector = (int)jnum(b, "phy-sec", 512);
    d.removable = jbool(b, "rm");
    d.read_only = jbool(b, "ro");
    if (d.table.empty()) {
      d.fs_type = jstr(b, "fstype");
      d.label = jstr(b, "label");
      d.mount_points = jmounts(b);
    }
    for (auto &c : b["children"].items()) {
      if (jstr(c, "type") != "part") continue;
      Partition p;
      p.device = jstr(c, "path");
      if (p.device.empty()) p.device = "/dev/" + jstr(c, "name");
      p.number = (int)jnum(c, "partn", 0);
      if (p.number <= 0) {  // older lsblk: the trailing digits of sdb1, nvme0n1p2
        std::string n = jstr(c, "name");
        size_t i = n.size();
        while (i > 0 && isdigit((unsigned char)n[i - 1])) i--;
        p.number = i < n.size() ? atoi(n.c_str() + i) : 0;
      }
      // START is in 512-byte sectors (sysfs), whatever the sector size
      p.start = (uint64_t)std::max<int64_t>(0, jnum(c, "start", 0)) * 512;
      p.size = (uint64_t)std::max<int64_t>(0, jnum(c, "size", 0));
      p.name = jstr(c, "partlabel");
      p.fs_type = jstr(c, "fstype");
      p.label = jstr(c, "label");
      p.uuid = jstr(c, "uuid");
      p.type = jstr(c, "parttype");
      p.type_name = jstr(c, "parttypename");
      p.flags = jstr(c, "partflags");
      p.mount_points = jmounts(c);
      p.fs_used = jnum(c, "fsused", -1);
      p.fs_size = jnum(c, "fssize", -1);
      p.extended = d.table == "dos" && is_extended_type(p.type);
      p.logical = d.table == "dos" && p.number >= 5;
      // the child list may hold device-mapper children of a partition, not more partitions
      d.partitions.push_back(p);
    }
    std::sort(d.partitions.begin(), d.partitions.end(),
              [](const Partition &a, const Partition &b) { return a.start < b.start; });
    // the kernel shows an extended partition as 1 KiB: span its logical ones
    for (auto &p : d.partitions)
      if (p.extended) {
        uint64_t end = p.start + kMiB;
        for (auto &q : d.partitions)
          if (q.logical) end = std::max(end, q.end());
        p.size = end - p.start;
      }
    compute_free(d);
    out.push_back(d);
  }
  std::sort(out.begin(), out.end(), [](const Disk &a, const Disk &b) {
    if (a.removable != b.removable) return !a.removable;
    return a.name < b.name;
  });
  return true;
}

void compute_free(Disk &d) {
  d.free.clear();
  if (d.table.empty()) return;
  auto add_gaps = [&](uint64_t from, uint64_t to, bool logical, const std::vector<const Partition *> &parts) {
    uint64_t pos = from;
    for (auto *p : parts) {
      if (p->start > pos) {
        uint64_t s = align_up(pos), e = align_down(std::min(p->start, to));
        if (e > s && e - s >= kMiB) d.free.push_back({s, e - s, logical});
      }
      pos = std::max(pos, p->end());
    }
    uint64_t s = align_up(pos), e = align_down(to);
    if (e > s && e - s >= kMiB) d.free.push_back({s, e - s, logical});
  };
  std::vector<const Partition *> top, inside;
  for (auto &p : d.partitions) (p.logical ? inside : top).push_back(&p);
  add_gaps(d.usable_start(), d.usable_end(), false, top);
  if (const Partition *ext = d.extended())
    // logical partitions need a little room before each for their EBR
    add_gaps(ext->start + kMiB, ext->end(), true, inside);
  std::sort(d.free.begin(), d.free.end(), [](const Region &a, const Region &b) { return a.start < b.start; });
}

// ---- parted -m ------------------------------------------------------------------

namespace {

// splits a parted machine line on unescaped ':' (drops the final ';')
std::vector<std::string> parted_fields(std::string line) {
  line = fld::trim(line);
  if (!line.empty() && line.back() == ';') line.pop_back();
  std::vector<std::string> out(1);
  for (size_t i = 0; i < line.size(); i++) {
    char c = line[i];
    if (c == '\\' && i + 1 < line.size()) {
      out.back() += line[++i];
    } else if (c == ':') {
      out.emplace_back();
    } else {
      out.back() += c;
    }
  }
  return out;
}

uint64_t bytes_field(const std::string &f) {
  std::string s = f;
  if (!s.empty() && s.back() == 'B') s.pop_back();
  return strtoull(s.c_str(), nullptr, 10);
}

}  // namespace

const PartedPart *PartedTable::part(int number) const {
  for (auto &p : parts)
    if (!p.free && p.number == number) return &p;
  return nullptr;
}

bool parse_parted(const std::string &text, PartedTable &out, std::string &err) {
  out = PartedTable();
  auto lines = fld::split(text, '\n');
  size_t i = 0;
  while (i < lines.size() && fld::trim(lines[i]) != "BYT;") i++;
  if (i >= lines.size()) {
    err = "parted printed no table in bytes";
    return false;
  }
  if (++i >= lines.size()) {
    err = "parted printed no device line";
    return false;
  }
  auto dev = parted_fields(lines[i]);
  if (dev.size() < 7) {
    err = "cannot read parted's device line";
    return false;
  }
  out.device = dev[0];
  out.size = bytes_field(dev[1]);
  out.transport = dev[2];
  out.logical_sector = atoi(dev[3].c_str());
  out.physical_sector = atoi(dev[4].c_str());
  out.table = dev[5];
  out.model = dev[6];
  for (i++; i < lines.size(); i++) {
    auto f = parted_fields(lines[i]);
    if (f.size() < 4 || f[0].empty() || !isdigit((unsigned char)f[0][0])) continue;
    PartedPart p;
    p.number = atoi(f[0].c_str());
    p.start = bytes_field(f[1]);
    p.end = bytes_field(f[2]);
    p.size = bytes_field(f[3]);
    if (f.size() >= 5 && f[4] == "free") {
      p.free = true;
      p.number = 0;
    } else {
      if (f.size() > 4) p.fs = f[4];
      if (f.size() > 5) p.name = f[5];
      if (f.size() > 6) p.flags = f[6];
    }
    out.parts.push_back(p);
  }
  return true;
}

// ---- file systems -----------------------------------------------------------------

const std::vector<FsInfo> &filesystems() {
  static const std::vector<FsInfo> fs = {
      {"ext4", "ext4 (Linux)", "mkfs.ext4", "ext4", 16, false, true, true, false, "resize2fs", "e2fsprogs"},
      {"btrfs", "Btrfs (Linux)", "mkfs.btrfs", "btrfs", 255, false, true, true, true, "btrfs", "btrfs-progs"},
      {"xfs", "XFS (Linux)", "mkfs.xfs", "xfs", 12, false, true, false, true, "xfs_growfs", "xfsprogs"},
      {"vfat", "FAT32 (all systems)", "mkfs.fat", "fat32", 11, true, false, false, false, "", "dosfstools"},
      {"exfat", "exFAT (all systems, large files)", "mkfs.exfat", "ntfs", 15, false, false, false, false, "",
       "exfatprogs"},
      {"ntfs", "NTFS (Windows)", "mkfs.ntfs", "ntfs", 128, false, true, true, false, "ntfsresize", "ntfs-3g"},
      {"swap", "Linux swap", "mkswap", "linux-swap", 15, false, false, false, false, "", "util-linux"},
  };
  return fs;
}

const FsInfo *fs_info(const std::string &id) {
  std::string i = fs_id_from_lsblk(id);
  for (auto &f : filesystems())
    if (i == f.id) return &f;
  return nullptr;
}

std::string fs_id_from_lsblk(const std::string &fstype) {
  std::string l = fld::lower(fstype);
  if (l == "vfat" || l == "fat32" || l == "fat16" || l == "fat") return "vfat";
  if (l == "ext4") return "ext4";
  if (l == "btrfs") return "btrfs";
  if (l == "xfs") return "xfs";
  if (l == "exfat") return "exfat";
  if (l == "ntfs" || l == "ntfs3") return "ntfs";
  if (l == "swap" || l == "linux-swap") return "swap";
  return "";
}

std::string fit_label(const FsInfo &fs, const std::string &label) {
  std::string out;
  for (unsigned char c : fld::trim(label)) {
    if (c < 0x20 || c == 0x7f) continue;
    if (fs.label_upper) {
      // FAT: plain ASCII, upper case, none of the characters it forbids
      if (c >= 0x80 || strchr("\"*/:<>?\\|+,.;=[]", c)) continue;
      out += (char)toupper(c);
    } else {
      out += (char)c;
    }
  }
  // cut at a character boundary (labels count bytes for most tools)
  size_t max = (size_t)fs.label_max;
  if (out.size() > max) {
    out.resize(max);
    // drop a character the cut split: find the last lead byte and its length
    size_t lead = out.size();
    while (lead > 0 && ((unsigned char)out[lead - 1] & 0xC0) == 0x80) lead--;
    if (lead > 0) {
      unsigned char c = (unsigned char)out[lead - 1];
      size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
      if (out.size() - (lead - 1) < len) out.resize(lead - 1);
    } else {
      out.clear();
    }
  }
  return out;
}

// ---- commands -----------------------------------------------------------------------

namespace {
std::string B(uint64_t v) { return std::to_string(v) + "B"; }
std::string quoted(const std::string &s) { return "\"" + s + "\""; }  // parted's own word quoting
}  // namespace

std::vector<std::string> parted_mklabel(const std::string &dev, const std::string &table) {
  return {"parted", "-s", dev, "mklabel", table == "dos" ? "msdos" : table};
}

std::vector<std::string> parted_mkpart(const std::string &dev, const std::string &table, const std::string &kind,
                                       const std::string &name, const std::string &fs, uint64_t start, uint64_t end) {
  const FsInfo *f = fs_info(fs);
  std::vector<std::string> v{"parted", "-s", "-a", "none", dev, "unit", "B", "mkpart"};
  if (table == "gpt") v.push_back(quoted(name));
  else v.push_back(kind.empty() ? "primary" : kind);
  if (kind != "extended") v.push_back(f ? f->parted_type : "ext4");
  v.push_back(B(start));
  v.push_back(B(end - 1));
  return v;
}

std::vector<std::string> parted_rm(const std::string &dev, int number) {
  return {"parted", "-s", dev, "rm", std::to_string(number)};
}

std::vector<std::string> parted_name(const std::string &dev, int number, const std::string &name) {
  return {"parted", "-s", dev, "name", std::to_string(number), quoted(name)};
}

std::vector<std::string> parted_set(const std::string &dev, int number, const std::string &flag, bool on) {
  return {"parted", "-s", dev, "set", std::to_string(number), flag, on ? "on" : "off"};
}

std::vector<std::string> parted_resizepart(const std::string &dev, int number, uint64_t end) {
  // ---pretend-input-tty: with -s parted refuses to shrink; the helper answers "Yes"
  return {"parted", "---pretend-input-tty", dev, "unit", "B", "resizepart", std::to_string(number), B(end - 1)};
}

std::vector<std::string> mkfs_command(const std::string &fs, const std::string &dev, const std::string &label) {
  const FsInfo *f = fs_info(fs);
  if (!f) return {};
  std::string l = fit_label(*f, label);
  std::string id = f->id;
  std::vector<std::string> v;
  if (id == "ext4") v = {"mkfs.ext4", "-F", "-q"};
  else if (id == "btrfs") v = {"mkfs.btrfs", "-f", "-q"};
  else if (id == "xfs") v = {"mkfs.xfs", "-f", "-q"};
  else if (id == "vfat") v = {"mkfs.fat", "-F", "32"};
  else if (id == "exfat") v = {"mkfs.exfat"};
  else if (id == "ntfs") v = {"mkfs.ntfs", "-Q", "-F", "-q"};
  else if (id == "swap") v = {"mkswap"};
  if (!l.empty()) {
    v.push_back(id == "vfat" ? "-n" : "-L");
    v.push_back(l);
  }
  v.push_back(dev);
  return v;
}

std::vector<std::string> partition_flags(const std::string &table) {
  if (table == "gpt") return {"esp", "bios_grub", "boot", "msftdata", "msftres", "hidden", "diag", "raid", "lvm", "swap", "legacy_boot"};
  return {"boot", "esp", "lba", "hidden", "raid", "lvm", "swap", "diag"};
}

std::vector<std::string> current_flags(const Partition &p, const std::string &table) {
  std::vector<std::string> out;
  std::string t = fld::lower(p.type);
  if (table == "gpt") {
    struct {
      const char *guid, *flag;
    } known[] = {{"c12a7328-f81f-11d2-ba4b-00a0c93ec93b", "esp"},
                 {"21686148-6449-6e6f-744e-656564454649", "bios_grub"},
                 {"0657fd6d-a4ab-43c4-84e5-0933c84b4f4f", "swap"},
                 {"e6d6d379-f507-44c2-a23c-238f2a3df928", "lvm"},
                 {"a19d880f-05fc-4d3b-a006-743f0f84911e", "raid"},
                 {"ebd0a0a2-b9e5-4433-87c0-68b6b72699c7", "msftdata"},
                 {"e3c9e316-0b5c-4db8-817d-f92df00215ae", "msftres"},
                 {"de94bba4-06d1-4d40-a16a-bfd50179d6ac", "diag"}};
    for (auto &k : known)
      if (t == k.guid) out.push_back(k.flag);
    // the EFI system partition is also "boot" to parted
    if (t == "c12a7328-f81f-11d2-ba4b-00a0c93ec93b") out.push_back("boot");
    // attribute bit 2: legacy BIOS bootable
    unsigned long long attrs = strtoull(p.flags.c_str(), nullptr, 16);
    if (attrs & 4) out.push_back("legacy_boot");
  } else {
    if (fld::lower(p.flags) == "0x80") out.push_back("boot");
    if (t == "0xef") out.push_back("esp");
    if (t == "0x82") out.push_back("swap");
    if (t == "0x8e") out.push_back("lvm");
    if (t == "0xfd") out.push_back("raid");
    if (t == "0x27") out.push_back("diag");
    if (t == "0xc" || t == "0x0c" || t == "0xe" || t == "0x0e" || t == "0xf" || t == "0x0f") out.push_back("lba");
    if (t.size() == 4 && t[2] == '1') out.push_back("hidden");  // 0x11, 0x1b, 0x1c, ...
  }
  return out;
}

bool valid_flag(const std::string &table, const std::string &flag) {
  auto f = partition_flags(table);
  return std::find(f.begin(), f.end(), flag) != f.end();
}

bool valid_partition_name(const std::string &name, std::string &err) {
  if (name.size() > 36 * 2) {  // 36 UTF-16 characters; keep UTF-8 bytes in check
    err = "The name is too long (36 characters at most).";
    return false;
  }
  for (unsigned char c : name)
    if (c < 0x20 || c == 0x7f || c == '"' || c == '\\') {
      err = "The name cannot have quotes, backslashes or control characters.";
      return false;
    }
  size_t chars = 0;
  for (unsigned char c : name) chars += (c & 0xC0) != 0x80;
  if (chars > 36) {
    err = "The name is too long (36 characters at most).";
    return false;
  }
  return true;
}

bool resize_plan(const std::string &disk, int number, const std::string &part_dev, const std::string &fs,
                 uint64_t start, uint64_t old_size, uint64_t new_size, std::vector<Step> &steps, std::string &err) {
  steps.clear();
  if (new_size == old_size) {
    err = "The size is the same.";
    return false;
  }
  if (new_size < kMiB || new_size % kAlign) {
    err = "The new size has to be a whole number of MiB.";
    return false;
  }
  bool grow = new_size > old_size;
  uint64_t end = start + new_size;
  Step part{Step::Run, parted_resizepart(disk, number, end), grow ? "Growing the partition" : "Shrinking the partition",
            grow ? "" : "Yes\n"};
  std::string id = fs_id_from_lsblk(fs);
  if (fs.empty()) {  // no file system: only the partition
    steps.push_back(part);
    return true;
  }
  const FsInfo *f = fs_info(id);
  if (!f || (grow && !f->grow) || (!grow && !f->shrink)) {
    std::string name = f ? f->title : fs;
    if (id == "swap") err = "Swap cannot be resized: delete it and create a new swap partition instead.";
    else if (f && grow && !f->shrink && !f->grow) err = name + " file systems cannot be resized.";
    else err = name + " file systems cannot be " + (grow ? "grown." : "shrunk.");
    return false;
  }
  std::string sz = std::to_string(new_size);
  if (id == "ext4") {
    Step check{Step::Run, {"e2fsck", "-f", "-y", part_dev}, "Checking the file system"};
    if (grow) {
      steps = {part, check, {Step::Run, {"resize2fs", part_dev}, "Growing the file system"}};
    } else {
      steps = {check, {Step::Run, {"resize2fs", part_dev, std::to_string(new_size / 1024) + "K"}, "Shrinking the file system"},
               part};
    }
  } else if (id == "ntfs") {
    if (grow) {
      steps = {part, {Step::Run, {"ntfsresize", "-f", "-b", "-P", part_dev}, "Growing the file system"}};
    } else {
      steps = {{Step::Run, {"ntfsresize", "-f", "-n", "-P", "-s", sz, part_dev}, "Checking the file system"},
               {Step::Run, {"ntfsresize", "-f", "-P", "-s", sz, part_dev}, "Shrinking the file system"},
               part};
    }
  } else if (id == "btrfs") {
    Step mount{Step::Mount, {"mount", "-t", "btrfs", part_dev, "@MP"}, "Mounting the file system"};
    Step umount{Step::Unmount, {"umount", "@MP"}, "Unmounting the file system"};
    if (grow) {
      steps = {part, mount, {Step::Run, {"btrfs", "filesystem", "resize", "max", "@MP"}, "Growing the file system"}, umount};
    } else {
      steps = {mount, {Step::Run, {"btrfs", "filesystem", "resize", sz, "@MP"}, "Shrinking the file system"}, umount, part};
    }
  } else if (id == "xfs") {
    steps = {part,
             {Step::Mount, {"mount", "-t", "xfs", part_dev, "@MP"}, "Mounting the file system"},
             {Step::Run, {"xfs_growfs", "@MP"}, "Growing the file system"},
             {Step::Unmount, {"umount", "@MP"}, "Unmounting the file system"}};
  } else {
    err = std::string(f->title) + " file systems cannot be resized.";
    return false;
  }
  return true;
}

uint64_t max_size(const Disk &d, const Partition &p) {
  uint64_t limit = d.usable_end();
  if (p.logical)
    if (const Partition *e = d.extended()) limit = e->end();
  for (auto &q : d.partitions)
    if (&q != &p && q.logical == p.logical && q.start >= p.end()) limit = std::min(limit, q.start);
  return limit > p.start ? align_down(limit - p.start) : p.size;
}

// ---- devices ------------------------------------------------------------------------

bool disk_name_ok(const std::string &name, bool allow_loop) {
  auto digits_from = [&](size_t i) {
    if (i >= name.size()) return false;
    for (; i < name.size(); i++)
      if (!isdigit((unsigned char)name[i])) return false;
    return true;
  };
  auto letters_from = [&](size_t i) {
    if (i >= name.size()) return false;
    for (; i < name.size(); i++)
      if (!islower((unsigned char)name[i])) return false;
    return true;
  };
  for (const char *p : {"sd", "vd", "hd", "xvd"})
    if (fld::starts_with(name, p) && letters_from(strlen(p))) return true;
  if (fld::starts_with(name, "mmcblk") && digits_from(6)) return true;
  if (fld::starts_with(name, "nvme")) {
    size_t n = name.find('n', 4);
    if (n != std::string::npos && n > 4) {
      bool ok = true;
      for (size_t i = 4; i < n; i++) ok = ok && isdigit((unsigned char)name[i]);
      if (ok && digits_from(n + 1)) return true;
    }
  }
  if (allow_loop && fld::starts_with(name, "loop") && digits_from(4)) return true;
  return false;
}

std::string partition_name(const std::string &disk, int number) {
  bool digit_end = !disk.empty() && isdigit((unsigned char)disk.back());
  return disk + (digit_end ? "p" : "") + std::to_string(number);
}

std::string size_text(uint64_t bytes) {
  const char *u[] = {"B", "kB", "MB", "GB", "TB", "PB"};
  double v = (double)bytes;
  int i = 0;
  while (v >= 1000 && i < 5) {
    v /= 1000;
    i++;
  }
  char buf[32];
  if (i == 0) snprintf(buf, sizeof buf, "%llu B", (unsigned long long)bytes);
  else snprintf(buf, sizeof buf, v < 10 ? "%.1f %s" : "%.0f %s", v, u[i]);
  return buf;
}

uint64_t parse_size(const std::string &text) {
  std::string t = fld::trim(text);
  if (t.empty()) return 0;
  char *end = nullptr;
  double v = strtod(t.c_str(), &end);
  if (end == t.c_str() || v <= 0 || !std::isfinite(v)) return 0;
  std::string unit = fld::lower(fld::trim(end));
  double m;
  if (unit.empty() || unit == "mib" || unit == "m") m = (double)kMiB;
  else if (unit == "gib" || unit == "g") m = 1024.0 * kMiB;
  else if (unit == "tib" || unit == "t") m = 1024.0 * 1024 * kMiB;
  else if (unit == "kib" || unit == "k") m = 1024;
  else if (unit == "mb") m = 1e6;
  else if (unit == "gb") m = 1e9;
  else if (unit == "tb") m = 1e12;
  else if (unit == "kb") m = 1e3;
  else if (unit == "b") m = 1;
  else return 0;
  double r = v * m;
  if (r > 1.8e19) return 0;
  return (uint64_t)llround(r);
}

bool parse_progress(const std::string &line, uint64_t &done, uint64_t &total) {
  unsigned long long a = 0, b = 0;
  if (sscanf(line.c_str(), "PROGRESS %llu %llu", &a, &b) != 2) return false;
  done = a;
  total = b;
  return true;
}

}  // namespace disks
