#include "archive_core.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <clocale>
#include <dirent.h>
#include <langinfo.h>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace arc {

// ---- names ------------------------------------------------------------------------------

const std::vector<FormatInfo> &formats() {
  static const std::vector<FormatInfo> f = {
      {ZIP, ".zip", "zip", true},
      {TAR_GZ, ".tar.gz", "tar.gz (gzip)", false},
      {TAR_XZ, ".tar.xz", "tar.xz (xz)", false},
      {TAR_ZST, ".tar.zst", "tar.zst (zstd)", false},
      {TAR_BZ2, ".tar.bz2", "tar.bz2 (bzip2)", false},
      {SEVEN_ZIP, ".7z", "7z", false},
      {TAR, ".tar", "tar (uncompressed)", false},
  };
  return f;
}

static std::string lower(std::string s) {
  for (auto &c : s) c = (char)tolower((unsigned char)c);
  return s;
}

static bool ends_with(const std::string &s, const std::string &e) {
  return s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0;
}

bool format_for_name(const std::string &name, Format &f) {
  std::string l = lower(name);
  static const std::pair<const char *, Format> exts[] = {
      {".tar.gz", TAR_GZ}, {".tgz", TAR_GZ}, {".tar.xz", TAR_XZ}, {".txz", TAR_XZ}, {".tar.zst", TAR_ZST},
      {".tzst", TAR_ZST}, {".tar.bz2", TAR_BZ2}, {".tbz2", TAR_BZ2}, {".tbz", TAR_BZ2}, {".tar", TAR},
      {".zip", ZIP}, {".7z", SEVEN_ZIP}};
  for (auto &e : exts)
    if (ends_with(l, e.first)) {
      f = e.second;
      return true;
    }
  return false;
}

static const char *kArchiveExts[] = {".tar.gz", ".tar.xz", ".tar.zst", ".tar.bz2", ".tar.lz", ".tar.lzma", ".tar.z",
                                     ".tgz", ".txz", ".tzst", ".tbz2", ".tbz", ".tar", ".zip", ".7z", ".rar",
                                     ".iso", ".cpio", ".xar", ".cab", ".lha", ".lzh", ".jar", ".apk", ".deb",
                                     ".rpm", ".ar", ".gz", ".xz", ".zst", ".bz2", ".lz4", ".lzma", ".z"};

std::string strip_archive_ext(const std::string &name) {
  std::string l = lower(name);
  for (const char *e : kArchiveExts)
    if (ends_with(l, e) && name.size() > strlen(e)) return name.substr(0, name.size() - strlen(e));
  size_t dot = name.rfind('.');
  return dot != std::string::npos && dot > 0 ? name.substr(0, dot) : name;
}

bool is_archive_name(const std::string &name) {
  std::string l = lower(name);
  for (const char *e : kArchiveExts)
    if (ends_with(l, e)) return true;
  return false;
}

std::string clean_path(const std::string &p) {
  std::string s = p;
  while (s.compare(0, 2, "./") == 0) s.erase(0, 2);
  while (!s.empty() && s.back() == '/') s.pop_back();
  return s;
}

bool safe_path(const std::string &p) {
  if (p.empty() || p[0] == '/') return false;
  size_t start = 0;
  while (start <= p.size()) {
    size_t e = p.find('/', start);
    std::string part = p.substr(start, e == std::string::npos ? std::string::npos : e - start);
    if (part == "..") return false;
    if (e == std::string::npos) break;
    start = e + 1;
  }
  return true;
}

std::string common_root(const std::vector<Entry> &entries) {
  std::string root;
  for (auto &e : entries) {
    std::string p = clean_path(e.path);
    if (p.empty()) continue;
    size_t slash = p.find('/');
    std::string top = slash == std::string::npos ? p : p.substr(0, slash);
    // a top-level file means there is no single folder
    if (slash == std::string::npos && !e.dir) return "";
    if (root.empty()) root = top;
    else if (root != top) return "";
  }
  return root;
}

// libarchive converts names between the archive's character set and the
// locale's: with the "C" locale, non-ASCII names fail. Use UTF-8.
static void utf8_locale() {
  static bool done = false;
  if (done) return;
  done = true;
  const char *cur = setlocale(LC_CTYPE, nullptr);
  if (cur && strcmp(cur, "C") && strcmp(cur, "POSIX")) {
    if (!strcmp(nl_langinfo(CODESET), "UTF-8")) return;
  }
  if (setlocale(LC_CTYPE, "") && !strcmp(nl_langinfo(CODESET), "UTF-8")) return;
  if (!setlocale(LC_CTYPE, "C.UTF-8")) setlocale(LC_CTYPE, "en_US.UTF-8");
}

// ---- reading ----------------------------------------------------------------------------

static struct archive *open_reader(const std::string &path, const std::string &password, std::string &err) {
  utf8_locale();
  struct archive *a = archive_read_new();
  archive_read_support_filter_all(a);
  archive_read_support_format_all(a);
  // single compressed files (file.txt.gz): read as one "raw" entry
  archive_read_support_format_raw(a);
  if (!password.empty()) archive_read_add_passphrase(a, password.c_str());
  if (archive_read_open_filename(a, path.c_str(), 64 * 1024) != ARCHIVE_OK) {
    err = archive_error_string(a) ? archive_error_string(a) : "cannot open the archive";
    archive_read_free(a);
    return nullptr;
  }
  return a;
}

static bool is_raw(struct archive *a) { return archive_format(a) == ARCHIVE_FORMAT_RAW; }

static std::string basename_of(const std::string &p) {
  size_t s = p.rfind('/');
  return s == std::string::npos ? p : p.substr(s + 1);
}

static bool password_error(struct archive *a) {
  const char *e = archive_error_string(a);
  if (!e) return false;
  std::string l = lower(e);
  return l.find("passphrase") != std::string::npos || l.find("password") != std::string::npos ||
         l.find("decrypt") != std::string::npos || l.find("encrypt") != std::string::npos;
}

Result list(const std::string &archive, std::vector<Entry> &out, const std::string &password) {
  Result r;
  out.clear();
  struct archive *a = open_reader(archive, password, r.error);
  if (!a) return r;
  struct archive_entry *e;
  int rc;
  while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
    Entry en;
    const char *p = archive_entry_pathname_utf8(e);
    if (!p) p = archive_entry_pathname(e);
    en.path = clean_path(p ? p : "");
    if (is_raw(a)) {
      // a plain compressed file: name it after the archive
      if (archive_filter_count(a) <= 1) {
        r.error = "This is not an archive fl-archive can read";
        archive_read_free(a);
        return r;
      }
      en.path = strip_archive_ext(basename_of(archive));
    }
    en.dir = archive_entry_filetype(e) == AE_IFDIR;
    en.symlink = archive_entry_filetype(e) == AE_IFLNK;
    if (en.symlink && archive_entry_symlink(e)) en.link = archive_entry_symlink(e);
    en.size = archive_entry_size_is_set(e) ? archive_entry_size(e) : 0;
    en.mtime = archive_entry_mtime_is_set(e) ? archive_entry_mtime(e) : 0;
    en.mode = archive_entry_perm(e);
    en.encrypted = archive_entry_is_encrypted(e);
    if (!en.path.empty()) out.push_back(en);
    if (is_raw(a)) {
      // the size of a raw entry is only known by reading it
      int64_t n = 0;
      char buf[65536];
      la_ssize_t got;
      while ((got = archive_read_data(a, buf, sizeof buf)) > 0) n += got;
      out.back().size = n;
    } else {
      archive_read_data_skip(a);
    }
  }
  if (rc != ARCHIVE_EOF) {
    r.need_password = password_error(a);
    r.error = archive_error_string(a) ? archive_error_string(a) : "cannot read the archive";
    archive_read_free(a);
    return r;
  }
  archive_read_free(a);
  r.ok = true;
  return r;
}

static bool selected(const std::string &path, const std::vector<std::string> &only) {
  if (only.empty()) return true;
  for (auto &o : only) {
    std::string c = clean_path(o);
    if (path == c || (path.size() > c.size() && path.compare(0, c.size(), c) == 0 && path[c.size()] == '/')) return true;
  }
  return false;
}

static bool mkdirs(const std::string &dir) {
  if (dir.empty()) return true;
  std::string cur;
  size_t start = 0;
  while (start <= dir.size()) {
    size_t e = dir.find('/', start);
    cur = dir.substr(0, e);
    if (!cur.empty() && mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
    if (e == std::string::npos) break;
    start = e + 1;
  }
  return true;
}

static int64_t file_size(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0 ? st.st_size : 0;
}

Result extract(const std::string &archive, const ExtractOptions &opt) {
  Result r;
  if (!mkdirs(opt.dest)) {
    r.error = "Cannot create " + opt.dest + ": " + strerror(errno);
    return r;
  }
  char real[PATH_MAX];
  std::string dest = realpath(opt.dest.c_str(), real) ? std::string(real) : opt.dest;
  struct archive *a = open_reader(archive, opt.password, r.error);
  if (!a) return r;
  struct archive *w = archive_write_disk_new();
  // entry paths are checked above (safe_path) and made absolute under dest;
  // libarchive still refuses ".." and writing through symlinks
  archive_write_disk_set_options(w, ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_SECURE_NODOTDOT |
                                        ARCHIVE_EXTRACT_SECURE_SYMLINKS);
  archive_write_disk_set_standard_lookup(w);
  uint64_t total = (uint64_t)file_size(archive);
  bool replace_all = false, skip_all = false;
  std::string strip = clean_path(opt.strip);
  struct archive_entry *e;
  int rc;
  while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
    const char *pp = archive_entry_pathname_utf8(e);
    if (!pp) pp = archive_entry_pathname(e);
    std::string path = clean_path(pp ? pp : "");
    if (is_raw(a)) path = strip_archive_ext(basename_of(archive));
    if (path.empty() || !selected(path, opt.only)) {
      archive_read_data_skip(a);
      continue;
    }
    std::string rel = path;
    if (!strip.empty()) {
      if (rel == strip) {
        archive_read_data_skip(a);
        continue;
      }
      if (rel.compare(0, strip.size() + 1, strip + "/") == 0) rel = rel.substr(strip.size() + 1);
    }
    if (!safe_path(rel)) {
      r.skipped.push_back(path);
      archive_read_data_skip(a);
      continue;
    }
    std::string full = dest + "/" + rel;
    if (opt.progress && !opt.progress((uint64_t)archive_filter_bytes(a, -1), total, rel)) {
      r.cancelled = true;
      break;
    }
    struct stat st;
    bool is_dir = archive_entry_filetype(e) == AE_IFDIR;
    if (!is_dir && lstat(full.c_str(), &st) == 0) {
      Conflict c = skip_all ? SKIP : replace_all ? REPLACE : opt.on_conflict ? opt.on_conflict(rel) : REPLACE;
      if (c == CANCEL) {
        r.cancelled = true;
        break;
      }
      if (c == SKIP_ALL) skip_all = true;
      if (c == REPLACE_ALL) replace_all = true;
      if (c == SKIP || c == SKIP_ALL) {
        r.skipped.push_back(rel);
        archive_read_data_skip(a);
        continue;
      }
      if (S_ISDIR(st.st_mode)) {
        r.skipped.push_back(rel);
        archive_read_data_skip(a);
        continue;
      }
      unlink(full.c_str());
    }
    archive_entry_set_pathname(e, full.c_str());
    // hard links point at other entries: move them under dest as well
    if (const char *hl = archive_entry_hardlink(e)) {
      std::string h = clean_path(hl);
      if (!strip.empty() && h.compare(0, strip.size() + 1, strip + "/") == 0) h = h.substr(strip.size() + 1);
      if (!safe_path(h)) {
        r.skipped.push_back(path);
        archive_read_data_skip(a);
        continue;
      }
      archive_entry_set_hardlink(e, (dest + "/" + h).c_str());
    }
    if (archive_write_header(w, e) != ARCHIVE_OK) {
      r.skipped.push_back(path + ": " + (archive_error_string(w) ? archive_error_string(w) : "error"));
      archive_read_data_skip(a);
      continue;
    }
    const void *buf;
    size_t size;
    la_int64_t off;
    int drc;
    while ((drc = archive_read_data_block(a, &buf, &size, &off)) == ARCHIVE_OK) {
      if (archive_write_data_block(w, buf, size, off) != ARCHIVE_OK) {
        r.error = archive_error_string(w) ? archive_error_string(w) : "write error";
        break;
      }
    }
    if (drc != ARCHIVE_EOF && r.error.empty()) {
      r.need_password = password_error(a) || archive_entry_is_encrypted(e);
      r.error = archive_error_string(a) ? archive_error_string(a) : "read error";
    }
    archive_write_finish_entry(w);
    if (!r.error.empty()) break;
  }
  if (rc != ARCHIVE_EOF && rc != ARCHIVE_OK && rc != ARCHIVE_WARN && r.error.empty() && !r.cancelled) {
    r.need_password = password_error(a);
    r.error = archive_error_string(a) ? archive_error_string(a) : "cannot read the archive";
  }
  archive_write_close(w);
  archive_write_free(w);
  archive_read_free(a);
  r.ok = r.error.empty() && !r.cancelled;
  return r;
}

// ---- writing ----------------------------------------------------------------------------

static struct archive *open_writer(const std::string &path, Format f, const std::string &password, std::string &err) {
  utf8_locale();
  struct archive *w = archive_write_new();
  int rc = ARCHIVE_OK;
  switch (f) {
    case ZIP: rc = archive_write_set_format_zip(w); break;
    case SEVEN_ZIP: rc = archive_write_set_format_7zip(w); break;
    default: rc = archive_write_set_format_pax_restricted(w); break;
  }
  if (rc == ARCHIVE_OK) {
    switch (f) {
      case TAR_GZ: rc = archive_write_add_filter_gzip(w); break;
      case TAR_XZ: rc = archive_write_add_filter_xz(w); break;
      case TAR_ZST: rc = archive_write_add_filter_zstd(w); break;
      case TAR_BZ2: rc = archive_write_add_filter_bzip2(w); break;
      default: break;
    }
  }
  if (rc == ARCHIVE_OK && !password.empty()) {
    if (f != ZIP) {
      err = "Only zip archives can have a password";
      archive_write_free(w);
      return nullptr;
    }
    rc = archive_write_set_options(w, "zip:encryption=aes256");
    if (rc == ARCHIVE_OK) rc = archive_write_set_passphrase(w, password.c_str());
  }
  if (rc == ARCHIVE_OK) rc = archive_write_open_filename(w, path.c_str());
  if (rc != ARCHIVE_OK) {
    err = archive_error_string(w) ? archive_error_string(w) : "cannot create the archive";
    archive_write_free(w);
    return nullptr;
  }
  return w;
}

struct WalkItem {
  std::string disk, name; // path on disk, name in the archive
};

static void walk(const std::string &disk, const std::string &name, std::vector<WalkItem> &out, uint64_t &bytes) {
  struct stat st;
  if (lstat(disk.c_str(), &st) != 0) return;
  out.push_back({disk, name});
  if (S_ISREG(st.st_mode)) bytes += (uint64_t)st.st_size;
  if (!S_ISDIR(st.st_mode)) return;
  DIR *d = opendir(disk.c_str());
  if (!d) return;
  std::vector<std::string> names;
  while (struct dirent *de = readdir(d))
    if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) names.push_back(de->d_name);
  closedir(d);
  std::sort(names.begin(), names.end());
  for (auto &n : names) walk(disk + "/" + n, name + "/" + n, out, bytes);
}

// Writes one item from disk. false with err on a write error.
static bool write_item(struct archive *w, const WalkItem &it, uint64_t &done, uint64_t total, const Progress &progress,
                       bool &cancelled, std::string &err) {
  struct stat st;
  if (lstat(it.disk.c_str(), &st) != 0) return true; // vanished: skip
  struct archive_entry *e = archive_entry_new();
  archive_entry_copy_stat(e, &st);
  archive_entry_set_pathname_utf8(e, it.name.c_str());
  archive_entry_set_uname(e, "");
  archive_entry_set_gname(e, "");
  if (S_ISLNK(st.st_mode)) {
    char buf[4096];
    ssize_t n = readlink(it.disk.c_str(), buf, sizeof buf - 1);
    if (n >= 0) {
      buf[n] = 0;
      archive_entry_set_symlink(e, buf);
    }
  }
  if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
    archive_entry_free(e); // devices, sockets: not archived
    return true;
  }
  if (archive_write_header(w, e) != ARCHIVE_OK) {
    err = it.name + ": " + (archive_error_string(w) ? archive_error_string(w) : "error");
    archive_entry_free(e);
    return false;
  }
  if (S_ISREG(st.st_mode)) {
    int fd = open(it.disk.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
      char buf[65536];
      ssize_t n;
      while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (archive_write_data(w, buf, (size_t)n) < 0) {
          err = archive_error_string(w) ? archive_error_string(w) : "write error";
          break;
        }
        done += (uint64_t)n;
        if (progress && !progress(done, total, it.name)) {
          cancelled = true;
          break;
        }
      }
      close(fd);
    }
  }
  archive_entry_free(e);
  return err.empty() && !cancelled;
}

static std::string temp_name(const std::string &archive) { return archive + ".part-" + std::to_string(getpid()); }

Result create(const std::string &archive, const std::vector<std::string> &files, const CreateOptions &opt) {
  Result r;
  std::vector<WalkItem> items;
  uint64_t total = 0, done = 0;
  for (auto &f : files) {
    std::string disk = f;
    while (disk.size() > 1 && disk.back() == '/') disk.pop_back();
    walk(disk, basename_of(disk), items, total);
  }
  if (items.empty()) {
    r.error = "Nothing to put into the archive";
    return r;
  }
  std::string tmp = temp_name(archive);
  struct archive *w = open_writer(tmp, opt.format, opt.password, r.error);
  if (!w) return r;
  for (auto &it : items) {
    // the archive itself, when it is created inside a folder being archived
    if (it.disk == archive || it.disk == tmp) continue;
    if (!write_item(w, it, done, total, opt.progress, r.cancelled, r.error)) break;
  }
  bool closed = archive_write_close(w) == ARCHIVE_OK;
  if (!closed && r.error.empty()) r.error = archive_error_string(w) ? archive_error_string(w) : "write error";
  archive_write_free(w);
  if (!r.error.empty() || r.cancelled) {
    unlink(tmp.c_str());
    return r;
  }
  if (rename(tmp.c_str(), archive.c_str()) != 0) {
    r.error = std::string("Cannot write ") + archive + ": " + strerror(errno);
    unlink(tmp.c_str());
    return r;
  }
  r.ok = true;
  return r;
}

// Copies the entries of `archive` into a new archive, leaving out `drop`
// (exact paths and folders), then appends `add_items`.
static Result rewrite(const std::string &archive, const std::vector<std::string> &drop,
                      const std::vector<WalkItem> &add_items, uint64_t add_bytes, const std::string &password,
                      const Progress &progress) {
  Result r;
  Format f;
  if (!format_for_name(archive, f)) {
    r.error = "fl-archive cannot change this kind of archive (only zip, 7z and tar archives)";
    return r;
  }
  struct archive *a = open_reader(archive, password, r.error);
  if (!a) return r;
  std::string tmp = temp_name(archive);
  // keep zip encryption when the password is known
  struct archive *w = open_writer(tmp, f, f == ZIP ? password : "", r.error);
  if (!w) {
    archive_read_free(a);
    return r;
  }
  uint64_t total = (uint64_t)file_size(archive) + add_bytes, done = 0;
  struct archive_entry *e;
  int rc;
  while ((rc = archive_read_next_header(a, &e)) == ARCHIVE_OK || rc == ARCHIVE_WARN) {
    const char *pp = archive_entry_pathname_utf8(e);
    if (!pp) pp = archive_entry_pathname(e);
    std::string path = clean_path(pp ? pp : "");
    bool skip = selected(path, drop) && !drop.empty();
    for (auto &it : add_items) skip |= it.name == path; // replaced by an added file
    if (skip) {
      archive_read_data_skip(a);
      continue;
    }
    if (progress && !progress((uint64_t)archive_filter_bytes(a, -1), total, path)) {
      r.cancelled = true;
      break;
    }
    if (archive_write_header(w, e) != ARCHIVE_OK) {
      r.error = path + ": " + (archive_error_string(w) ? archive_error_string(w) : "error");
      break;
    }
    char buf[65536];
    la_ssize_t n;
    while ((n = archive_read_data(a, buf, sizeof buf)) > 0) {
      if (archive_write_data(w, buf, (size_t)n) < 0) {
        r.error = archive_error_string(w) ? archive_error_string(w) : "write error";
        break;
      }
    }
    if (n < 0 && r.error.empty()) {
      r.need_password = password_error(a) || archive_entry_is_encrypted(e);
      r.error = archive_error_string(a) ? archive_error_string(a) : "read error";
    }
    if (!r.error.empty()) break;
  }
  if (r.error.empty() && !r.cancelled && rc != ARCHIVE_EOF && rc != ARCHIVE_OK && rc != ARCHIVE_WARN) {
    r.need_password = password_error(a);
    r.error = archive_error_string(a) ? archive_error_string(a) : "cannot read the archive";
  }
  done = (uint64_t)file_size(archive);
  if (r.error.empty() && !r.cancelled)
    for (auto &it : add_items)
      if (!write_item(w, it, done, total, progress, r.cancelled, r.error)) break;
  bool closed = archive_write_close(w) == ARCHIVE_OK;
  if (!closed && r.error.empty()) r.error = archive_error_string(w) ? archive_error_string(w) : "write error";
  archive_write_free(w);
  archive_read_free(a);
  if (!r.error.empty() || r.cancelled) {
    unlink(tmp.c_str());
    return r;
  }
  // keep the archive's permissions
  struct stat st;
  if (stat(archive.c_str(), &st) == 0) chmod(tmp.c_str(), st.st_mode & 07777);
  if (rename(tmp.c_str(), archive.c_str()) != 0) {
    r.error = std::string("Cannot write ") + archive + ": " + strerror(errno);
    unlink(tmp.c_str());
    return r;
  }
  r.ok = true;
  return r;
}

Result add(const std::string &archive, const std::vector<std::string> &files, const std::string &into,
           const std::string &password, Progress progress) {
  std::vector<WalkItem> items;
  uint64_t bytes = 0;
  std::string prefix = clean_path(into);
  for (auto &f : files) {
    std::string disk = f;
    while (disk.size() > 1 && disk.back() == '/') disk.pop_back();
    std::string name = basename_of(disk);
    walk(disk, prefix.empty() ? name : prefix + "/" + name, items, bytes);
  }
  if (items.empty()) {
    Result r;
    r.error = "Nothing to add";
    return r;
  }
  return rewrite(archive, {}, items, bytes, password, progress);
}

Result remove(const std::string &archive, const std::vector<std::string> &entries, const std::string &password,
              Progress progress) {
  if (entries.empty()) {
    Result r;
    r.ok = true;
    return r;
  }
  return rewrite(archive, entries, {}, 0, password, progress);
}

} // namespace arc
