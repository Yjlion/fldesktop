// fl-archive core: list, extract, create and change archives with libarchive.
// No FLTK; the functions are synchronous (fl-archive runs them in a thread).
#pragma once
#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <vector>

namespace arc {

struct Entry {
  std::string path; // as stored, without a leading "./" and trailing "/"
  int64_t size = 0;
  time_t mtime = 0;
  bool dir = false, symlink = false, encrypted = false;
  std::string link;
  unsigned mode = 0644;
};

enum Format { TAR_GZ, TAR_XZ, TAR_ZST, TAR_BZ2, TAR, ZIP, SEVEN_ZIP };
struct FormatInfo {
  Format format;
  const char *ext;   // ".tar.gz"
  const char *label; // "tar.gz (gzip)"
  bool password;     // can be encrypted when created
};
const std::vector<FormatInfo> &formats();
// The format a file name asks for (by extension); false when unknown.
bool format_for_name(const std::string &name, Format &f);
// "photos.tar.gz" -> "photos", "a.zip" -> "a", "file.txt.gz" -> "file.txt"
std::string strip_archive_ext(const std::string &name);
// Whether the name looks like an archive or a compressed file fl-archive opens.
bool is_archive_name(const std::string &name);

// Entry paths that would escape the destination ("../x", "/etc/x") are unsafe.
bool safe_path(const std::string &p);
// "./a/b/" -> "a/b"
std::string clean_path(const std::string &p);
// The single top-level folder of all entries ("" when there are several
// top-level items or a top-level file).
std::string common_root(const std::vector<Entry> &entries);

// Progress: return false to cancel. `total` may be 0 (unknown).
typedef std::function<bool(uint64_t done, uint64_t total, const std::string &current)> Progress;

struct Result {
  bool ok = false;
  bool cancelled = false;
  bool need_password = false; // encrypted and no / a wrong password
  std::string error;
  std::vector<std::string> skipped; // unsafe or skipped entries
};

Result list(const std::string &archive, std::vector<Entry> &out, const std::string &password = "");

enum Conflict { REPLACE, SKIP, REPLACE_ALL, SKIP_ALL, CANCEL };
struct ExtractOptions {
  std::string dest;                 // folder, created when missing
  std::vector<std::string> only;    // entries (folders include their contents); empty = all
  std::string strip;                // a leading folder to remove from the paths ("" = none)
  std::string password;
  std::function<Conflict(const std::string &path)> on_conflict; // default: REPLACE
  Progress progress;
};
Result extract(const std::string &archive, const ExtractOptions &opt);

struct CreateOptions {
  Format format = TAR_GZ;
  std::string password;            // zip only
  Progress progress;
};
// Stores each of `files` (files or folders, recursively) under its base name.
Result create(const std::string &archive, const std::vector<std::string> &files, const CreateOptions &opt);

// Adds files (under `into`, a folder inside the archive or "") / removes
// entries (folders with their contents). The archive is rewritten in the same
// format through a temporary file.
Result add(const std::string &archive, const std::vector<std::string> &files, const std::string &into,
           const std::string &password, Progress progress);
Result remove(const std::string &archive, const std::vector<std::string> &entries, const std::string &password,
              Progress progress);

} // namespace arc
