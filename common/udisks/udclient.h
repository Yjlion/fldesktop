// UDisks2 access for fldesktop: volumes to show, mount, unmount, unlock,
// eject (GDBus on the fld-glib thread).
//
// The UI sees plain snapshots (State); actions report back in the FLTK
// thread. Call Fl::lock() once in main().
// FLD_UDISKS_BUS=session makes the client use the session bus (tests).
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace ud {

struct Volume {
  std::string path;          // UDisks2 block object path
  std::string device;        // /dev/sdb1
  std::string label;         // name to show
  std::string icon;          // icon name
  std::string fs_type;       // vfat, ext4, crypto_LUKS, ...
  std::string mount_point;   // first mount point, "" when not mounted
  std::string drive;         // drive object path ("" for loop devices etc.)
  std::string drive_name;    // vendor + model
  std::string cleartext;     // encrypted volume: the unlocked block device ("" while locked)
  std::string backing;       // unlocked volume: the encrypted block it comes from
  unsigned long long size = 0;
  bool removable = false;    // on a removable drive (USB stick, SD card, optical)
  bool can_eject = false;    // the drive can be ejected or powered off
  bool encrypted = false;    // a locked or unlocked LUKS container (has no file system itself)
  bool read_only = false;
  bool hint_auto = false;    // UDisks suggests mounting it automatically
  bool mounted() const { return !mount_point.empty(); }
};

// A drive with its health (SMART) as UDisks reports it.
struct Drive {
  std::string path;             // UDisks2 drive object path
  std::string device;           // its whole-disk block device, /dev/sda
  std::string name;             // vendor + model
  std::string serial;
  bool smart_supported = false; // ATA SMART or NVMe health data is available
  bool smart_enabled = false;
  bool smart_failing = false;   // the drive predicts it will fail
  bool nvme = false;
  double temperature_c = 0;     // 0 = unknown
  unsigned long long power_on_seconds = 0;
  long long bad_sectors = -1;   // -1 = unknown
  std::vector<std::string> nvme_warnings; // NVMe SmartCriticalWarning: spare, temperature, degraded, readonly, ...
  bool healthy() const { return !smart_failing && nvme_warnings.empty() && bad_sectors <= 0; }
};

struct State {
  bool running = false;          // udisksd is on the bus
  std::vector<Volume> volumes;   // removable first, then by name
  std::vector<Drive> drives;     // drives with a block device, by device
  const Drive *drive_for_device(const std::string &dev) const;
  const Volume *volume(const std::string &path) const;
  const Volume *by_mount_point(const std::string &mp) const;
};

using Done = std::function<void(const std::string &error)>;                          // "" = success
using DoneValue = std::function<void(const std::string &error, const std::string &value)>;

class Client {
public:
  static Client &get();
  void start();
  const State &state() const { return state_; }
  std::function<void()> on_changed;                 // FLTK thread
  std::function<void(const Volume &)> on_added;     // FLTK thread: a volume appeared after start

  void mount(const std::string &volume, DoneValue done = nullptr);    // value: the mount point
  void unmount(const std::string &volume, Done done = nullptr);
  void unlock(const std::string &volume, const std::string &passphrase, DoneValue done = nullptr); // value: cleartext path
  void lock(const std::string &volume, Done done = nullptr);
  // Unmounts every file system of the volume's drive, locks its encrypted
  // volumes, then ejects the medium or powers the drive off.
  void eject(const std::string &volume, Done done = nullptr);

  State state_; // internal: written in the FLTK thread only
private:
  Client() = default;
};

// helpers (unit tested)
std::string error_text(const std::string &dbus_error_name, const std::string &message);
// The name to show: hint name, file system label, else "<size> Volume".
std::string volume_label(const std::string &hint_name, const std::string &id_label, unsigned long long size);
// One line about a drive's health: "Healthy · 34 °C · powered on 1.2 years",
// "FAILING: back up its data now", "" when no data.
std::string health_text(const Drive &d);
// Whether a block device is shown as a volume.
bool volume_shown(const std::string &id_usage, bool hint_ignore, bool hint_system, bool mounted,
                  const std::string &mount_point);

} // namespace ud
