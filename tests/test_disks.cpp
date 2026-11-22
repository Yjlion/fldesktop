// fl-disks core: lsblk and parted parsing, free space, commands, resize plans
#include "disks.h"
#include "fld/util.h"
#include "test.h"

using namespace disks;

static const char *kGpt = R"({
 "blockdevices": [
  {"name":"sda","path":"/dev/sda","type":"disk","size":53687091200,"start":null,"phy-sec":4096,"log-sec":512,
   "model":"QEMU HARDDISK","serial":"drive-scsi0","tran":"sata","rm":false,"ro":false,"pttype":"gpt","partn":null,
   "fstype":null,"label":null,"mountpoints":[null],
   "children":[
    {"name":"sda2","path":"/dev/sda2","type":"part","size":314572800,"start":4096,"pttype":"gpt","partn":2,
     "parttype":"c12a7328-f81f-11d2-ba4b-00a0c93ec93b","parttypename":"EFI System","partlabel":"EFI","partflags":null,
     "fstype":"vfat","label":"ESP","uuid":"1234-ABCD","mountpoints":["/boot"],"fsused":"52428800","fssize":"314572800"},
    {"name":"sda1","path":"/dev/sda1","type":"part","size":1048576,"start":2048,"pttype":"gpt","partn":1,
     "parttype":"21686148-6449-6e6f-744e-656564454649","parttypename":"BIOS boot","partlabel":"BIOS boot partition",
     "partflags":null,"fstype":null,"mountpoints":[]},
    {"name":"sda3","path":"/dev/sda3","type":"part","size":10737418240,"start":1048576,"pttype":"gpt","partn":3,
     "parttype":"0fc63daf-8483-4772-8e79-3d69d8477de4","partlabel":"root","partflags":"0x4",
     "fstype":"ext4","label":"root","mountpoints":["/","/home"],"fsused":1000,"fssize":2000}
   ]},
  {"name":"loop0","path":"/dev/loop0","type":"loop","size":1048576,"children":[]},
  {"name":"sr0","path":"/dev/sr0","type":"rom","size":1048576},
  {"name":"zram0","path":"/dev/zram0","type":"disk","size":1048576},
  {"name":"sdb","path":"/dev/sdb","type":"disk","size":16000000000,"model":"USB Stick","tran":"usb","rm":true,
   "ro":false,"pttype":"dos","children":[
    {"name":"sdb1","path":"/dev/sdb1","type":"part","size":1073741824,"start":2048,"partn":1,"parttype":"0xc",
     "partflags":"0x80","fstype":"vfat","label":"STICK","mountpoints":[]},
    {"name":"sdb2","path":"/dev/sdb2","type":"part","size":1024,"start":4196352,"partn":2,"parttype":"0xf"},
    {"name":"sdb5","path":"/dev/sdb5","type":"part","size":1073741824,"start":4198400,"partn":5,"parttype":"0x83",
     "fstype":"ext4"}
   ]},
  {"name":"nvme0n1","path":"/dev/nvme0n1","type":"disk","size":512110190592,"tran":"nvme","pttype":null,
   "fstype":"btrfs","label":"whole","mountpoints":["/data"]}
 ]})";

int main() {
  // ---- lsblk
  std::vector<Disk> ds;
  std::string err;
  CHECK(parse_lsblk(kGpt, ds, false, &err));
  CHECK_EQ(ds.size(), 3u);  // sda, nvme0n1, sdb (removable last); no loop, rom or zram
  CHECK_EQ(ds[0].name, std::string("nvme0n1"));
  CHECK_EQ(ds[1].name, std::string("sda"));
  CHECK_EQ(ds[2].name, std::string("sdb"));
  const Disk &a = ds[1];
  CHECK_EQ(a.table_name(), std::string("GPT"));
  CHECK_EQ(a.physical_sector, 4096);
  CHECK_EQ(a.partitions.size(), 3u);
  CHECK_EQ(a.partitions[0].number, 1);  // sorted by start
  CHECK_EQ(a.partitions[1].start, 4096ull * 512);
  CHECK_EQ(a.partitions[1].fs_used, 52428800);  // numbers as strings
  CHECK_EQ(a.partitions[2].mount_points.size(), 2u);
  CHECK(a.mount_points.empty());  // [null]
  CHECK(a.any_mounted());
  CHECK(a.partition(3) && a.partition(3)->label == "root");
  CHECK_EQ(a.partitions[2].title(), std::string("sda3 · root"));
  // free: between sda2 (ends at 302 MiB) and sda3 (512 MiB), and after sda3
  // to the end, minus the GPT backup
  CHECK_EQ(a.free.size(), 2u);
  CHECK_EQ(a.free[0].start, 302 * kMiB);
  CHECK_EQ(a.free[0].end(), 512 * kMiB);
  CHECK_EQ(a.free[1].start, 1048576ull * 512 + 10737418240ull);
  CHECK_EQ(a.free[1].end(), a.usable_end());
  CHECK_EQ(a.usable_end() % kMiB, 0ull);
  CHECK(a.usable_end() < a.size);
  // flags
  auto f1 = current_flags(a.partitions[0], "gpt");
  CHECK_EQ(f1.size(), 1u);
  CHECK_EQ(f1[0], std::string("bios_grub"));
  auto f2 = current_flags(a.partitions[1], "gpt");
  CHECK_EQ(f2.size(), 2u);
  CHECK_EQ(f2[0], std::string("esp"));
  auto f3 = current_flags(a.partitions[2], "gpt");
  CHECK_EQ(f3.size(), 1u);
  CHECK_EQ(f3[0], std::string("legacy_boot"));

  const Disk &b = ds[2];
  CHECK(b.removable);
  CHECK_EQ(b.table_name(), std::string("MBR (DOS)"));
  CHECK(b.extended() && b.extended()->number == 2);
  CHECK(b.partition(5)->logical);
  CHECK(!b.partition(1)->logical);
  // the extended one spans its logical partitions
  CHECK_EQ(b.extended()->end(), b.partition(5)->end());
  auto fb = current_flags(*b.partition(1), "dos");
  CHECK_EQ(fb.size(), 2u);  // boot, lba
  // free: between sdb1 and the extended one, and after it
  CHECK_EQ(b.free.size(), 2u);
  CHECK_EQ(b.free[0].start, 1048576ull + 1073741824ull);
  CHECK(!b.free[0].logical);
  CHECK_EQ(b.free[1].start, b.partition(5)->end());

  const Disk &n = ds[0];
  CHECK(n.table.empty());
  CHECK_EQ(n.fs_type, std::string("btrfs"));
  CHECK(n.any_mounted());
  CHECK(n.free.empty());
  CHECK_EQ(n.table_name(), std::string("None"));

  // older lsblk without PARTN: numbers from the names
  {
    std::vector<Disk> old;
    CHECK(parse_lsblk(R"({"blockdevices":[{"name":"nvme0n1","type":"disk","size":1073741824,"pttype":"gpt",
      "children":[{"name":"nvme0n1p12","type":"part","start":2048,"size":1048576}]}]})", old));
    CHECK_EQ(old[0].partitions[0].number, 12);
    auto lc = lsblk_command(true);
    CHECK(lc.back().find("PARTN") == std::string::npos && lc.back().find("PARTTYPE") != std::string::npos);
  }
  CHECK(parse_lsblk(kGpt, ds, true));
  CHECK_EQ(ds.size(), 4u);  // with loop0
  CHECK(!parse_lsblk("{nope", ds));

  // an empty disk with a new table: one region from 1 MiB
  {
    Disk e;
    e.table = "gpt";
    e.size = 8ull << 30;
    compute_free(e);
    CHECK_EQ(e.free.size(), 1u);
    CHECK_EQ(e.free[0].start, kMiB);
    CHECK_EQ(e.free[0].end(), align_down(e.size - 33 * 512));
    e.table = "dos";
    compute_free(e);
    CHECK_EQ(e.free[0].end(), e.size);
  }

  // ---- max size
  {
    CHECK(parse_lsblk(kGpt, ds));
    const Disk &g = ds[1];
    CHECK_EQ(max_size(g, *g.partition(3)), g.usable_end() - g.partition(3)->start);
    CHECK_EQ(max_size(g, *g.partition(1)), g.partition(2)->start - g.partition(1)->start);
    const Disk &m = ds[2];
    CHECK_EQ(max_size(m, *m.partition(1)), m.extended()->start - m.partition(1)->start);
    CHECK_EQ(max_size(m, *m.partition(5)), m.partition(5)->size);  // the extended one ends there
  }

  // ---- parted -m
  {
    std::string text = "BYT;\n"
                       "/dev/sdb:16000000000B:usb:512:512:gpt:SanDisk Cruzer:;\n"
                       "1:17408B:1048575B:1031168B:free;\n"
                       "1:1048576B:105906175B:104857600B:ext4:my\\:data:;\n"
                       "2:105906176B:210763775B:104857600B:fat32:EFI:boot, esp;\n"
                       "1:210763776B:15999983103B:15789219328B:free;\n";
    PartedTable pt;
    CHECK(parse_parted(text, pt, err));
    CHECK_EQ(pt.device, std::string("/dev/sdb"));
    CHECK_EQ(pt.size, 16000000000ull);
    CHECK_EQ(pt.table, std::string("gpt"));
    CHECK_EQ(pt.model, std::string("SanDisk Cruzer"));
    CHECK_EQ(pt.parts.size(), 4u);
    CHECK(pt.parts[0].free && pt.parts[0].number == 0);
    CHECK(pt.part(1) && pt.part(1)->name == "my:data");
    CHECK_EQ(pt.part(1)->end, 105906175ull);
    CHECK_EQ(pt.part(2)->flags, std::string("boot, esp"));
    CHECK(!pt.part(3));
    CHECK(!parse_parted("Error: no such device\n", pt, err));
  }

  // ---- commands
  {
    auto v = parted_mkpart("/dev/sdb", "gpt", "", "my data", "vfat", 1 * kMiB, 101 * kMiB);
    CHECK_EQ(fld::join(v, " "), std::string("parted -s -a none /dev/sdb unit B mkpart \"my data\" fat32 1048576B 105906175B"));
    v = parted_mkpart("/dev/sdb", "dos", "logical", "ignored", "swap", 1 * kMiB, 2 * kMiB);
    CHECK_EQ(fld::join(v, " "), std::string("parted -s -a none /dev/sdb unit B mkpart logical linux-swap 1048576B 2097151B"));
    v = parted_mkpart("/dev/sdb", "gpt", "", "", "exfat", 1 * kMiB, 2 * kMiB);
    CHECK_EQ(v[8], std::string("\"\""));
    CHECK_EQ(v[9], std::string("ntfs"));  // exFAT: basic data / 0x07
    CHECK_EQ(fld::join(parted_mklabel("/dev/sdb", "dos"), " "), std::string("parted -s /dev/sdb mklabel msdos"));
    CHECK_EQ(fld::join(parted_rm("/dev/sdb", 3), " "), std::string("parted -s /dev/sdb rm 3"));
    CHECK_EQ(fld::join(parted_name("/dev/sdb", 1, "x y"), " "), std::string("parted -s /dev/sdb name 1 \"x y\""));
    CHECK_EQ(fld::join(parted_set("/dev/sdb", 2, "esp", false), " "), std::string("parted -s /dev/sdb set 2 esp off"));
    CHECK_EQ(fld::join(parted_resizepart("/dev/sdb", 1, 10 * kMiB), " "),
             std::string("parted ---pretend-input-tty /dev/sdb unit B resizepart 1 10485759B"));
    CHECK_EQ(fld::join(mkfs_command("ext4", "/dev/sdb1", "Data"), " "), std::string("mkfs.ext4 -F -q -L Data /dev/sdb1"));
    CHECK_EQ(fld::join(mkfs_command("vfat", "/dev/sdb1", "my stick.x"), " "), std::string("mkfs.fat -F 32 -n MY STICKX /dev/sdb1"));
    CHECK_EQ(fld::join(mkfs_command("swap", "/dev/sdb1", ""), " "), std::string("mkswap /dev/sdb1"));
    CHECK(mkfs_command("zfs", "/dev/sdb1", "").empty());
    CHECK_EQ(fit_label(*fs_info("ext4"), "a-very-long-label-indeed"), std::string("a-very-long-labe"));
    CHECK_EQ(fit_label(*fs_info("xfs"), "ññññññññ"), std::string("ññññññ"));  // 12 bytes, whole characters
    CHECK_EQ(fit_label(*fs_info("xfs"), "aññññññññ"), std::string("añññññ"));  // the cut splits the 6th ñ
    CHECK(fs_info("vfat") && fs_info("fat32") == fs_info("vfat"));
    CHECK_EQ(fs_id_from_lsblk("ntfs3"), std::string("ntfs"));
    CHECK_EQ(fs_id_from_lsblk("crypto_LUKS"), std::string(""));
    CHECK(valid_partition_name("EFI system", err));
    CHECK(!valid_partition_name("a\"b", err));
    CHECK(!valid_partition_name(std::string(37, 'x'), err));
    CHECK(valid_flag("gpt", "esp") && valid_flag("dos", "lba") && !valid_flag("dos", "bios_grub") && !valid_flag("gpt", "x;y"));
  }

  // ---- resize plans
  {
    std::vector<Step> st;
    CHECK(resize_plan("/dev/sdb", 1, "/dev/sdb1", "ext4", kMiB, 100 * kMiB, 50 * kMiB, st, err));
    CHECK_EQ(st.size(), 3u);  // check, shrink the fs, then the partition
    CHECK_EQ(st[0].argv[0], std::string("e2fsck"));
    CHECK_EQ(fld::join(st[1].argv, " "), std::string("resize2fs /dev/sdb1 51200K"));
    CHECK_EQ(st[2].argv[0], std::string("parted"));
    CHECK_EQ(st[2].input, std::string("Yes\n"));
    CHECK_EQ(st[2].argv.back(), std::to_string(51 * kMiB - 1) + "B");
    CHECK(resize_plan("/dev/sdb", 1, "/dev/sdb1", "ext4", kMiB, 100 * kMiB, 200 * kMiB, st, err));
    CHECK_EQ(st[0].argv[0], std::string("parted"));  // grow: the partition first
    CHECK(st[0].input.empty());
    CHECK_EQ(fld::join(st[2].argv, " "), std::string("resize2fs /dev/sdb1"));
    CHECK(resize_plan("/dev/sdb", 2, "/dev/sdb2", "btrfs", kMiB, 300 * kMiB, 400 * kMiB, st, err));
    CHECK_EQ(st.size(), 4u);
    CHECK(st[1].kind == Step::Mount && st[3].kind == Step::Unmount);
    CHECK(resize_plan("/dev/sdb", 2, "/dev/sdb2", "btrfs", kMiB, 400 * kMiB, 300 * kMiB, st, err));
    CHECK(st[0].kind == Step::Mount && st[3].argv[0] == "parted");
    CHECK_EQ(st[1].argv[3], std::to_string(300 * kMiB));
    CHECK(resize_plan("/dev/sdb", 1, "/dev/sdb1", "xfs", kMiB, 100 * kMiB, 200 * kMiB, st, err));
    CHECK(!resize_plan("/dev/sdb", 1, "/dev/sdb1", "xfs", kMiB, 200 * kMiB, 100 * kMiB, st, err));
    CHECK(err.find("shrunk") != std::string::npos);
    CHECK(!resize_plan("/dev/sdb", 1, "/dev/sdb1", "vfat", kMiB, 100 * kMiB, 200 * kMiB, st, err));
    CHECK(!resize_plan("/dev/sdb", 1, "/dev/sdb1", "swap", kMiB, 100 * kMiB, 200 * kMiB, st, err));
    CHECK(err.find("Swap") == 0);
    CHECK(!resize_plan("/dev/sdb", 1, "/dev/sdb1", "crypto_LUKS", kMiB, 100 * kMiB, 200 * kMiB, st, err));
    CHECK(resize_plan("/dev/sdb", 1, "/dev/sdb1", "", kMiB, 100 * kMiB, 20 * kMiB, st, err));
    CHECK_EQ(st.size(), 1u);  // no file system: only the partition
    CHECK(resize_plan("/dev/sdb", 1, "/dev/sdb1", "ntfs", kMiB, 100 * kMiB, 50 * kMiB, st, err));
    CHECK_EQ(st.size(), 3u);
    CHECK(!resize_plan("/dev/sdb", 1, "/dev/sdb1", "ext4", kMiB, 100 * kMiB, 100 * kMiB, st, err));
    CHECK(!resize_plan("/dev/sdb", 1, "/dev/sdb1", "ext4", kMiB, 100 * kMiB, 100 * kMiB + 5, st, err));
  }

  // ---- devices and sizes
  {
    CHECK(disk_name_ok("sda", false) && disk_name_ok("vdb", false) && disk_name_ok("nvme0n1", false));
    CHECK(disk_name_ok("mmcblk0", false) && disk_name_ok("xvda", false));
    CHECK(!disk_name_ok("sda1", false) && !disk_name_ok("nvme0n1p1", false) && !disk_name_ok("mmcblk0p1", false));
    CHECK(!disk_name_ok("loop0", false) && disk_name_ok("loop0", true));
    CHECK(!disk_name_ok("dm-0", true) && !disk_name_ok("../sda", true) && !disk_name_ok("", true));
    CHECK(!disk_name_ok("nvmen1", false) && !disk_name_ok("zram0", true) && !disk_name_ok("sr0", true));
    CHECK_EQ(partition_name("sdb", 1), std::string("sdb1"));
    CHECK_EQ(partition_name("nvme0n1", 2), std::string("nvme0n1p2"));
    CHECK_EQ(partition_name("mmcblk0", 1), std::string("mmcblk0p1"));
    CHECK_EQ(size_text(0), std::string("0 B"));
    CHECK_EQ(size_text(1500000000), std::string("1.5 GB"));
    CHECK_EQ(size_text(53687091200ull), std::string("54 GB"));
    CHECK_EQ(parse_size("512"), 512 * kMiB);
    CHECK_EQ(parse_size("20 GiB"), 20 * 1024 * kMiB);
    CHECK_EQ(parse_size("1.5g"), 1536 * kMiB);
    CHECK_EQ(parse_size("2 GB"), 2000000000ull);
    CHECK_EQ(parse_size(""), 0ull);
    CHECK_EQ(parse_size("-5"), 0ull);
    CHECK_EQ(parse_size("12 parsecs"), 0ull);
    uint64_t d0 = 0, t0 = 0;
    CHECK(parse_progress("PROGRESS 10 20", d0, t0) && d0 == 10 && t0 == 20);
    CHECK(!parse_progress("STEP x", d0, t0));
    CHECK_EQ(align_up(kMiB + 1), 2 * kMiB);
    CHECK_EQ(align_down(kMiB + 1), kMiB);
  }
  return test_result("disks");
}
