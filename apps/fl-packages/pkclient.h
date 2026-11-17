// PackageKit access for fl-packages (packagekit-glib2 on the fld-glib thread).
//
// Every call is asynchronous; callbacks run in the FLTK thread. A Job is one
// PackageKit transaction; its progress callback reports status and percent.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pk {

struct Package {
  std::string id;          // name;version;arch;data
  std::string name, version, arch, repo, summary;
  int info = 0;            // PkInfoEnum
  bool installed() const;
  bool is_update() const;  // security / bugfix / enhancement / normal update
};

struct Details {
  std::string id, summary, description, license, url, group;
  uint64_t size = 0, download_size = 0;
};

struct UpdateDetail {
  std::string id, update_text, changelog, issued;
  std::vector<std::string> cves, bugzilla, vendor_urls;
  int restart = 0;         // PkRestartEnum
};

struct Repo {
  std::string id, description;
  bool enabled = false;
};

struct PastTransaction {
  std::string tid, when, role, data, cmdline;
  bool succeeded = false;
  unsigned duration_ms = 0;
};

struct Progress {
  int percent = -1;        // -1 unknown
  std::string status;      // human readable
  std::string package;     // package being worked on (name)
  bool allow_cancel = true;
};

struct Error {
  std::string text;        // "" = success
  int code = 0;            // PkErrorEnum (0 = none), or -1 = canceled
  bool untrusted() const;  // retry without "only trusted" may work
  explicit operator bool() const { return !text.empty(); }
};

// a running transaction; cancel() asks PackageKit to stop it
class Job {
public:
  void cancel();
  struct Impl;
  std::shared_ptr<Impl> impl;
};

using ProgressFn = std::function<void(const Progress &)>;

bool available(); // packagekitd can be reached (synchronous, quick)
std::string backend_name();

Job get_updates(std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p = nullptr);
Job refresh_cache(bool force, std::function<void(const Error &)> done, ProgressFn p = nullptr);
// search terms in names (details=false) or descriptions; installed: 1 only installed, 0 only available, -1 both
Job search(const std::string &terms, bool details, int installed,
           std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p = nullptr);
Job search_group(const std::string &group, int installed,
                 std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p = nullptr);
Job installed_packages(std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p = nullptr);
Job get_details(const std::vector<std::string> &ids, std::function<void(const std::vector<Details> &, const Error &)> done);
Job get_update_detail(const std::vector<std::string> &ids,
                      std::function<void(const std::vector<UpdateDetail> &, const Error &)> done);
// simulate=true: the packages the change would touch (info tells install/remove/update)
Job install(const std::vector<std::string> &ids, bool simulate, bool only_trusted,
            std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p = nullptr);
Job remove(const std::vector<std::string> &ids, bool simulate, bool autoremove,
           std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p = nullptr);
Job update(const std::vector<std::string> &ids, bool simulate, bool only_trusted,
           std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p = nullptr);
// a local package file (.deb, .pkg.tar.zst)
Job install_files(const std::vector<std::string> &paths, bool simulate, bool only_trusted,
                  std::function<void(const std::vector<Package> &, const Error &)> done, ProgressFn p = nullptr);
// the files of (installed) packages, sorted
Job get_files(const std::vector<std::string> &ids,
              std::function<void(const std::vector<std::string> &, const Error &)> done);
// what the packages need / what needs them (installed and available packages)
Job depends_on(const std::vector<std::string> &ids, bool recursive,
               std::function<void(const std::vector<Package> &, const Error &)> done);
Job required_by(const std::vector<std::string> &ids, bool recursive,
                std::function<void(const std::vector<Package> &, const Error &)> done);
Job get_repos(std::function<void(const std::vector<Repo> &, const Error &)> done);
Job set_repo_enabled(const std::string &id, bool on, std::function<void(const Error &)> done);
Job get_history(unsigned max, std::function<void(const std::vector<PastTransaction> &, const Error &)> done);
// seconds since the cache was last refreshed (or a large number)
void time_since_refresh(std::function<void(unsigned)> done);

// ---- helpers (unit tested) --------------------------------------------------
std::string status_text(int pk_status);
std::string info_text(int pk_info);      // "Security update", "Installed", ...
std::string role_text(const std::string &role);
std::vector<std::string> groups();       // PackageKit group ids offered for browsing
std::string group_text(const std::string &group);
// splits "name;version;arch;data"
Package parse_id(const std::string &id);
// files containing `text` (case-insensitive); all when text is empty
std::vector<std::string> filter_files(const std::vector<std::string> &files, const std::string &text);
// one entry per package name (an installed one wins), sorted by name
std::vector<Package> unique_by_name(const std::vector<Package> &list);
// one line per change of a past transaction's data ("installing\tid\n...")
std::vector<std::string> history_lines(const std::string &data);

} // namespace pk
