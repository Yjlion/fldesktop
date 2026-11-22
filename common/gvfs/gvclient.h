// GVfs access for fldesktop: network shares (smb, sftp, ftp, WebDAV, nfs...),
// phones (MTP) and cameras (gPhoto2) through GIO.
//
// Locations are mounted with GIO and browsed through gvfs-fuse, which shows
// every GVfs mount as a folder under $XDG_RUNTIME_DIR/gvfs, so the programs
// read and write them with ordinary file system calls.
//
// GIO runs on the fld-glib thread; the UI sees plain snapshots (State) and
// the callbacks run in the FLTK thread. Call Fl::lock() once in main().
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace gv {

struct Mount {
  std::string name;          // to show ("share on server")
  std::string uri;           // its root: smb://server/share/
  std::string path;          // its root through gvfs-fuse ("" without gvfs-fuse)
  std::string icon;          // icon name
  bool can_unmount = false;
  bool can_eject = false;
};

// A volume GVfs offers that is not mounted yet (a phone, a camera). Drives
// with a block device are left to UDisks.
struct Volume {
  std::string id;            // to mount it with
  std::string name;
  std::string icon;
};

struct State {
  bool running = false;      // GVfs is there (its volume monitor answers)
  std::vector<Mount> mounts;
  std::vector<Volume> volumes;
  // the mount `path` is in ("" when it is not under a GVfs mount)
  const Mount *mount_for_path(const std::string &path) const;
};

// an entry of a GVfs location that has no fuse path (network:///)
struct NetEntry {
  std::string name;
  std::string icon;
  std::string target;        // the location it stands for (smb://server/)
};

// Asked in the FLTK thread while a location mounts
struct PasswordRequest {
  std::string message;
  std::string user, domain;  // proposed
  bool need_user = false, need_domain = false, need_password = true;
  bool anonymous_ok = false; // may log in anonymously
  bool can_save = false;     // the password can be remembered (keyring)
};
struct PasswordReply {
  bool ok = false;           // false: cancelled
  bool anonymous = false;
  std::string user, domain, password;
  enum Remember { NEVER, SESSION, FOREVER } remember = NEVER;
};

using Done = std::function<void(const std::string &error)>;                          // "" = success
using DoneValue = std::function<void(const std::string &error, const std::string &value)>;

// error text of an operation the user cancelled (no message needed)
extern const char *const kCancelled;

class Client {
public:
  static Client &get();
  void start();
  const State &state() const { return state_; }
  std::function<void()> on_changed; // FLTK thread

  // Dialogs (FLTK thread, modal). Without them the mount is cancelled.
  std::function<PasswordReply(const PasswordRequest &)> ask_password;
  // returns the index of the chosen answer, -1 to cancel
  std::function<int(const std::string &message, const std::vector<std::string> &choices)> ask_question;

  // Mounts the location of uri (if needed); the value is its local path
  // (through gvfs-fuse).
  void mount_uri(const std::string &uri, DoneValue done);
  // Mounts a Volume; the value is the local path of its root.
  void mount_volume(const std::string &id, DoneValue done);
  // Unmounts (or ejects) the mount whose root is uri.
  void unmount(const std::string &uri, bool eject, Done done);
  // Lists a location that has no local path (network:///).
  void list(const std::string &uri, std::function<void(const std::string &error, std::vector<NetEntry>)> done);
  // Reads the mounts again.
  void refresh();

  State state_; // internal: written in the FLTK thread only
};

// ---- helpers without GIO (tests use them) ----

// $XDG_RUNTIME_DIR/gvfs, where gvfs-fuse shows the mounts
std::string fuse_root();
// path is under fuse_root()
bool is_fuse_path(const std::string &path, const std::string &root = fuse_root());
// "smb://server/share", "sftp://user@host:22/dir" ... (not file:// and trash:)
bool looks_like_uri(const std::string &s);
// the scheme of a URI ("smb"), "" when it has none
std::string uri_scheme(const std::string &uri);
// a name for a location: "share on server", "host", "user@host"
std::string display_name_for_uri(const std::string &uri);
// the URI of a local path under a mount (root uri + the rest, escaped)
std::string uri_for_path(const State &s, const std::string &path);

} // namespace gv
