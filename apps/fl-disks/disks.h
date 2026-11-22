// fl-disks core: the disks and their partitions (from lsblk, readable
// without root), parted's machine output, and the commands fl-disks-helper
// runs (parted, mkfs.*, resize tools). No UI, no root: unit tested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace disks {

constexpr uint64_t kMiB = 1024 * 1024;
constexpr uint64_t kAlign = kMiB;  // partitions start and end on 1 MiB

struct Partition {
  std::string device;      // /dev/sdb1
  std::string name;        // GPT partition name (PARTLABEL)
  std::string fs_type;     // ext4, vfat, swap, crypto_LUKS, ... ("" = none known)
  std::string label;       // file system label
  std::string uuid;
  std::string type;        // GPT type GUID or MBR type ("0x83")
  std::string type_name;   // "Linux filesystem", "EFI System", ...
  std::string flags;       // MBR: "0x80" for bootable
  int number = 0;
  uint64_t start = 0, size = 0;  // bytes
  std::vector<std::string> mount_points;
  int64_t fs_used = -1, fs_size = -1;  // bytes, -1 = unknown (not mounted)
  bool extended = false;   // MBR extended container
  bool logical = false;    // MBR logical partition (inside the extended one)
  uint64_t end() const { return start + size; }  // exclusive
  bool mounted() const { return !mount_points.empty(); }
  std::string title() const;  // "sdb1 · DATA"
};

struct Region {  // free space
  uint64_t start = 0, size = 0;
  bool logical = false;  // inside the extended partition
  uint64_t end() const { return start + size; }
};

struct Disk {
  std::string name;        // sdb
  std::string device;      // /dev/sdb
  std::string model, serial, transport;  // transport: usb, sata, nvme, ...
  std::string table;       // "gpt", "dos" or "" (none)
  std::string fs_type;     // a file system on the whole disk (no table)
  std::string label;
  std::vector<std::string> mount_points;  // of a whole-disk file system
  uint64_t size = 0;
  int logical_sector = 512, physical_sector = 512;
  bool removable = false, read_only = false;
  std::vector<Partition> partitions;  // by start
  std::vector<Region> free;           // gaps of at least 1 MiB
  const Partition *partition(int number) const;
  const Partition *extended() const;
  bool any_mounted() const;
  std::string title() const;          // model, else the device name
  std::string table_name() const;     // "GPT", "MBR (DOS)", "None"
  // the first and last byte (exclusive) partitions may use
  uint64_t usable_start() const;
  uint64_t usable_end() const;
};

// Parses `lsblk -J -b -o <kLsblkColumns>`. Loop, ram, zram and rom devices
// are left out (loop devices are kept with with_loop). Returns false on bad JSON.
extern const char *kLsblkColumns;
// legacy: without the columns util-linux < 2.39 does not know (PARTN); the
// partition numbers then come from the device names
std::vector<std::string> lsblk_command(bool legacy = false);
bool parse_lsblk(const std::string &json, std::vector<Disk> &out, bool with_loop = false, std::string *err = nullptr);
// Fills disk.free from the partitions.
void compute_free(Disk &d);

// `parted -m -s <dev> unit B print free`
struct PartedPart {
  int number = 0;  // 0 for free space
  uint64_t start = 0, end = 0, size = 0;  // end inclusive, as parted prints it
  std::string fs, name, flags;
  bool free = false;
};
struct PartedTable {
  std::string device, transport, table, model;
  uint64_t size = 0;
  int logical_sector = 512, physical_sector = 512;
  std::vector<PartedPart> parts;  // partitions and free space, by start
  const PartedPart *part(int number) const;
};
bool parse_parted(const std::string &text, PartedTable &out, std::string &err);

uint64_t align_up(uint64_t v, uint64_t a = kAlign);
uint64_t align_down(uint64_t v, uint64_t a = kAlign);

// ---- file systems -------------------------------------------------------------

struct FsInfo {
  const char *id;          // what the UI and the helper call it: ext4, btrfs, xfs, vfat, exfat, ntfs, swap
  const char *title;       // "ext4 (Linux)"
  const char *tool;        // the mkfs program
  const char *parted_type; // parted's fs-type for mkpart
  int label_max;           // characters
  bool label_upper;        // FAT labels are upper case
  bool grow, shrink;       // resizable this way
  bool needs_mount;        // resized while mounted (btrfs, xfs)
  const char *resize_tool; // "" when it cannot be resized
  const char *package;     // where the tools come from (shown when missing)
};
const std::vector<FsInfo> &filesystems();
const FsInfo *fs_info(const std::string &id);  // also knows lsblk's "vfat", "swap"
// The fs id fl-disks uses for an lsblk FSTYPE ("" when it is not one of ours).
std::string fs_id_from_lsblk(const std::string &fstype);
// Makes a label fit: cut to the limit, FAT upper case and no odd characters.
std::string fit_label(const FsInfo &fs, const std::string &label);

// ---- commands -----------------------------------------------------------------
// argv vectors with absolute byte positions (parted "unit B", inclusive ends).

std::vector<std::string> parted_mklabel(const std::string &dev, const std::string &table);  // gpt | msdos
// kind for MBR: primary, extended or logical; ignored for GPT (name is used)
std::vector<std::string> parted_mkpart(const std::string &dev, const std::string &table, const std::string &kind,
                                       const std::string &name, const std::string &fs, uint64_t start, uint64_t end);
std::vector<std::string> parted_rm(const std::string &dev, int number);
std::vector<std::string> parted_name(const std::string &dev, int number, const std::string &name);
std::vector<std::string> parted_set(const std::string &dev, int number, const std::string &flag, bool on);
std::vector<std::string> parted_resizepart(const std::string &dev, int number, uint64_t end);
// mkfs (after wipefs) for fs id; empty when unknown
std::vector<std::string> mkfs_command(const std::string &fs, const std::string &part_dev, const std::string &label);

// Flags parted can set, by table type.
std::vector<std::string> partition_flags(const std::string &table);
// The parted flags a partition has, as far as lsblk shows them (its type and
// the MBR boot flag): esp, bios_grub, boot, swap, lvm, raid, msftdata, ...
std::vector<std::string> current_flags(const Partition &p, const std::string &table);
// A GPT partition name parted accepts: printable, no quotes, at most 36 characters.
bool valid_partition_name(const std::string &name, std::string &err);
bool valid_flag(const std::string &table, const std::string &flag);

// One step of a resize. argv may hold "@MP": the temporary mount point.
struct Step {
  enum Kind { Run, Mount, Unmount } kind = Run;
  std::vector<std::string> argv;
  std::string what;   // shown while it runs
  std::string input;  // written to its stdin
};
// Steps to give partition `number` (device part_dev, file system fs id or
// "" when it has none) of `disk` a new size. Shrinking shrinks the file
// system first; growing grows the partition first. Returns false and sets
// err when this file system cannot be resized that way.
bool resize_plan(const std::string &disk, int number, const std::string &part_dev, const std::string &fs,
                 uint64_t start, uint64_t old_size, uint64_t new_size, std::vector<Step> &steps, std::string &err);
// The largest size partition `number` of `d` can grow to (to the next
// partition or the end of the usable space).
uint64_t max_size(const Disk &d, const Partition &p);

// ---- devices ------------------------------------------------------------------

// Whether a kernel block device name is a disk fl-disks shows and changes:
// sd*, vd*, hd*, nvme*n*, mmcblk*, xvd* (and loop* with allow_loop).
bool disk_name_ok(const std::string &name, bool allow_loop);
// "sdb" + 1 -> "sdb1", "nvme0n1" + 2 -> "nvme0n1p2", "mmcblk0" + 1 -> "mmcblk0p1"
std::string partition_name(const std::string &disk, int number);

// Human sizes for the UI: 1.5 GB (powers of 1000, as drives are sold) and
// MiB values for the size fields.
std::string size_text(uint64_t bytes);
// "512 MiB", "20 GiB", "1.5 TB" -> bytes; "" or junk -> 0. Plain numbers are MiB.
uint64_t parse_size(const std::string &text);

// "PROGRESS 123 456" -> done/total
bool parse_progress(const std::string &line, uint64_t &done, uint64_t &total);

}  // namespace disks
