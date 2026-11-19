// fl-archive core (libarchive): names, round trips, path safety, add/remove,
// passwords
#include <archive.h>
#include <archive_entry.h>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#include "archive_core.h"
#include "fld/util.h"
#include "test.h"

using namespace arc;

static std::string read(const std::string &p) {
  std::string s;
  fld::read_file(p, s);
  return s;
}

static bool has(const std::vector<Entry> &v, const std::string &p) {
  for (auto &e : v)
    if (e.path == p) return true;
  return false;
}

int main() {
  // names
  Format f;
  CHECK(format_for_name("a.tar.gz", f) && f == TAR_GZ);
  CHECK(format_for_name("A.TGZ", f) && f == TAR_GZ);
  CHECK(format_for_name("x.zip", f) && f == ZIP);
  CHECK(format_for_name("x.7z", f) && f == SEVEN_ZIP);
  CHECK(!format_for_name("x.rar", f));
  CHECK_EQ(strip_archive_ext("photos.tar.gz"), std::string("photos"));
  CHECK_EQ(strip_archive_ext("notes.txt.gz"), std::string("notes.txt"));
  CHECK_EQ(strip_archive_ext("a.zip"), std::string("a"));
  CHECK(is_archive_name("x.tar.zst"));
  CHECK(!is_archive_name("x.txt"));
  CHECK(safe_path("a/b"));
  CHECK(!safe_path("../a"));
  CHECK(!safe_path("a/../../b"));
  CHECK(!safe_path("/etc/passwd"));
  CHECK(safe_path("a..b/c"));
  CHECK_EQ(clean_path("./a/b/"), std::string("a/b"));
  CHECK_EQ(common_root({{"top", 0, 0, true}, {"top/a", 1}, {"top/b/c", 1}}), std::string("top"));
  CHECK_EQ(common_root({{"top/a", 1}, {"other/b", 1}}), std::string());
  CHECK_EQ(common_root({{"file", 1}}), std::string());

  std::string base = "/tmp/fld-test-archive-" + std::to_string(getpid());
  std::string src = base + "/src/Project";
  fld::mkdir_p(src + "/sub dir");
  fld::write_file(src + "/readme.txt", "hello\n");
  fld::write_file(src + "/sub dir/data ä.bin", std::string(200000, 'x'));
  if (symlink("readme.txt", (src + "/link").c_str()) != 0) fprintf(stderr, "no symlink\n");

  for (auto &fi : formats()) {
    std::string out = base + "/out" + fi.ext;
    CreateOptions co;
    co.format = fi.format;
    Result r = create(out, {src}, co);
    CHECK(r.ok);
    if (!r.ok) fprintf(stderr, "%s: %s\n", fi.ext, r.error.c_str());
    std::vector<Entry> list_out;
    r = arc::list(out, list_out);
    CHECK(r.ok);
    CHECK(has(list_out, "Project/readme.txt"));
    CHECK(has(list_out, "Project/sub dir/data ä.bin"));
    CHECK_EQ(common_root(list_out), std::string("Project"));
    std::string dest = base + "/x" + fi.ext;
    ExtractOptions eo;
    eo.dest = dest;
    r = extract(out, eo);
    CHECK(r.ok);
    CHECK_EQ(read(dest + "/Project/readme.txt"), std::string("hello\n"));
    CHECK_EQ(read(dest + "/Project/sub dir/data ä.bin").size(), (size_t)200000);
    // the folder stripped, one entry only
    ExtractOptions eo2;
    eo2.dest = dest + "-only";
    eo2.only = {"Project/sub dir"};
    eo2.strip = "Project";
    r = extract(out, eo2);
    CHECK(r.ok);
    CHECK(fld::file_exists(dest + "-only/sub dir/data ä.bin"));
    CHECK(!fld::file_exists(dest + "-only/readme.txt"));
  }

  // add and remove (zip and tar.gz)
  for (const char *ext : {".zip", ".tar.gz"}) {
    std::string out = base + "/out" + ext;
    fld::write_file(base + "/new.txt", "new");
    Result r = add(out, {base + "/new.txt"}, "Project/sub dir", "", nullptr);
    CHECK(r.ok);
    std::vector<Entry> l;
    arc::list(out, l);
    CHECK(has(l, "Project/sub dir/new.txt"));
    CHECK(has(l, "Project/readme.txt"));
    r = arc::remove(out, {"Project/sub dir"}, "", nullptr);
    CHECK(r.ok);
    arc::list(out, l);
    CHECK(!has(l, "Project/sub dir/new.txt"));
    CHECK(!has(l, "Project/sub dir/data ä.bin"));
    CHECK(has(l, "Project/readme.txt"));
  }

  // conflicts: skip keeps the existing file
  {
    std::string dest = base + "/conflict";
    fld::mkdir_p(dest + "/Project");
    fld::write_file(dest + "/Project/readme.txt", "mine");
    ExtractOptions eo;
    eo.dest = dest;
    int asked = 0;
    eo.on_conflict = [&](const std::string &) { asked++; return SKIP; };
    Result r = extract(base + "/out.tar.gz", eo);
    CHECK(r.ok);
    CHECK(asked >= 1);
    CHECK_EQ(read(dest + "/Project/readme.txt"), std::string("mine"));
  }

  // an archive with "../" entries: they are skipped, nothing lands outside
  {
    std::string evil = base + "/evil.tar";
    struct archive *w = archive_write_new();
    archive_write_set_format_pax_restricted(w);
    archive_write_open_filename(w, evil.c_str());
    for (const char *p : {"../escaped.txt", "ok.txt"}) {
      struct archive_entry *e = archive_entry_new();
      archive_entry_set_pathname(e, p);
      archive_entry_set_filetype(e, AE_IFREG);
      archive_entry_set_perm(e, 0644);
      archive_entry_set_size(e, 2);
      archive_write_header(w, e);
      archive_write_data(w, "hi", 2);
      archive_entry_free(e);
    }
    archive_write_close(w);
    archive_write_free(w);
    ExtractOptions eo;
    eo.dest = base + "/evil-out";
    Result r = extract(evil, eo);
    CHECK(r.ok);
    CHECK_EQ(r.skipped.size(), (size_t)1);
    CHECK(!fld::file_exists(base + "/escaped.txt"));
    CHECK(fld::file_exists(base + "/evil-out/ok.txt"));
  }

  // a zip with a password
  {
    std::string out = base + "/secret.zip";
    CreateOptions co;
    co.format = ZIP;
    co.password = "pw";
    Result r = create(out, {src + "/readme.txt"}, co);
    CHECK(r.ok);
    std::vector<Entry> l;
    arc::list(out, l);
    CHECK(!l.empty() && l[0].encrypted);
    ExtractOptions eo;
    eo.dest = base + "/secret-out";
    r = extract(out, eo);
    CHECK(!r.ok);
    CHECK(r.need_password);
    eo.password = "pw";
    r = extract(out, eo);
    CHECK(r.ok);
    CHECK_EQ(read(base + "/secret-out/readme.txt"), std::string("hello\n"));
    CreateOptions bad;
    bad.format = TAR_GZ;
    bad.password = "pw";
    CHECK(!create(base + "/x.tar.gz", {src}, bad).ok); // only zip has passwords
  }

  // a single compressed file
  {
    std::string gz = base + "/notes.txt.gz";
    CHECK(system(("printf 'abc' | gzip > '" + gz + "'").c_str()) == 0);
    std::vector<Entry> l;
    Result r = arc::list(gz, l);
    CHECK(r.ok);
    CHECK_EQ(l.size(), (size_t)1);
    if (!l.empty()) {
      CHECK_EQ(l[0].path, std::string("notes.txt"));
      CHECK_EQ(l[0].size, (int64_t)3);
    }
    ExtractOptions eo;
    eo.dest = base + "/gz-out";
    CHECK(extract(gz, eo).ok);
    CHECK_EQ(read(base + "/gz-out/notes.txt"), std::string("abc"));
  }

  // cancelling
  {
    CreateOptions co;
    co.format = TAR_XZ;
    co.progress = [](uint64_t, uint64_t, const std::string &) { return false; };
    Result r = create(base + "/cancel.tar.xz", {src}, co);
    CHECK(r.cancelled);
    CHECK(!fld::file_exists(base + "/cancel.tar.xz"));
  }

  CHECK(system(("rm -rf '" + base + "'").c_str()) == 0);
  return test_result("archive");
}
