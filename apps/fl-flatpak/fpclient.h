// Flatpak access for fl-flatpak (libflatpak + libappstream).
//
// Every call runs in a worker thread and reports in the FLTK thread
// (call Fl::lock() in main()). Transactions report progress as they go.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "remotes.h"

namespace fp {

struct App {
  std::string id, name, summary, description, developer, license, version, homepage;
  std::string icon;            // a local icon file ("" = none)
  std::string icon_stock;      // an icon theme name (AppStream stock icon)
  std::string icon_url;        // a remote icon (Flathub media), downloaded on demand
  std::string ref;             // app/<id>/<arch>/<branch>
  std::string remote;          // remote it comes from (or is installed from)
  std::string installation;    // "system" / "user" when installed
  std::vector<std::string> categories;
  bool installed = false, update = false, in_catalog = false;
  bool runtime = false;        // an installed runtime or extension, not an app
  uint64_t installed_size = 0;
};

struct Progress {
  int percent = -1;
  std::string status;          // "Installing org.foo.App (2/3)"
};

struct Catalog {
  std::vector<App> apps;       // catalog apps merged with installed ones, by name
  std::vector<std::string> remotes;
  bool have_flathub = false;
  std::string error;
};

class Job {
public:
  void cancel();
  std::shared_ptr<std::atomic<bool>> canceled = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<void> cancellable; // GCancellable
};

using Done = std::function<void(const std::string &error)>; // "" = success
using ProgressFn = std::function<void(const Progress &)>;

// refresh: download the remotes' AppStream data first; done() may move the
// catalog away
void load_catalog(bool refresh, std::function<void(Catalog &)> done, ProgressFn progress = nullptr);
Job install(const App &app, bool user, ProgressFn progress, Done done);
Job uninstall(const App &app, ProgressFn progress, Done done);
Job update(const std::vector<App> &apps, ProgressFn progress, Done done);
// a .flatpakref / .flatpakrepo / .flatpak file
Job install_file(const std::string &path, bool user, ProgressFn progress, Done done);
void add_flathub(bool user, Done done);
void launch(const App &app, Done done);
// the app's metadata keyfile (for its permissions); "" when not installed
std::string metadata(const App &app);
std::string overrides_path(const std::string &app_id); // the user's override file

// files, dependencies and dependents of an installed app or runtime
struct RefInfo {
  std::vector<std::string> files;  // relative to the deploy directory's files/
  bool files_truncated = false;
  std::vector<std::string> depends, used_by;  // refs; " (not installed)" appended when missing
  std::string error;
};
void ref_info(const App &app, std::function<void(const RefInfo &)> done);

// ---- sources (remotes) ------------------------------------------------------

void list_remotes(std::function<void(std::vector<Remote> &, const std::string &error)> done);
// url: a repository URL or a .flatpakrepo (file or https). gpg_key: a key
// file to trust, "" for the key in a .flatpakrepo, "-" for none (unsigned)
void add_remote(const std::string &installation, const std::string &name, const std::string &url,
                const std::string &title, const std::string &gpg_key, Done done);
// force: also when apps are installed from it (they stop getting updates)
void remove_remote(const Remote &r, bool force, Done done);
// changes title, priority and enabled state to those of r
void modify_remote(const Remote &r, Done done);

// ---- maintenance --------------------------------------------------------------
// runtimes and extensions no installed app needs any more
Job remove_unused(const std::string &installation, ProgressFn progress, Done done);
// flatpak repair (the system installation asks for an administrator)
void repair(const std::string &installation, Done done);

// helpers (unit tested)
// metadata keyfile -> "runtime/<name>/<arch>/<branch>" of an app ("" if none)
std::string metadata_runtime(const std::string &keyfile);
// metadata keyfile -> the ref an extension belongs to ([ExtensionOf] ref)
std::string metadata_extension_of(const std::string &keyfile);
std::string ref_id(const std::string &ref); // app/org.foo/x86_64/stable -> org.foo
std::string strip_markup(const std::string &appstream_description);
std::string category_label(const std::string &category);
const std::vector<std::string> &browse_categories();

} // namespace fp
