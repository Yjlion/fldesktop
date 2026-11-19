// Drag and drop rules (fld/fileops.h)
#include <FL/Fl.H>

#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "fld/fileops.h"
#include "fld/trash.h"
#include "fld/util.h"
#include "test.h"

int main() {
  char tmpl[] = "/tmp/fld-fileops-XXXXXX";
  std::string root = mkdtemp(tmpl);
  std::string a = root + "/a dir", b = root + "/b", f = root + "/a dir/file 1.txt";
  mkdir(a.c_str(), 0755);
  mkdir(b.c_str(), 0755);
  fld::write_file(f, "x");

  // uri lists: CRLF, percent encoding, localhost, comments, missing files, duplicates
  std::string text = "# comment\r\nfile:///" + fld::uri_encode_path(f).substr(1) + "\r\n" +
                     "file://localhost" + fld::uri_encode_path(a) + "/\r\n" + b + "\n" +
                     "file:///no/such/file\nhttp://example.com/x\n" + b + "\n";
  auto paths = fld::parse_uri_list(text);
  CHECK_EQ(paths.size(), (size_t)3);
  if (paths.size() == 3) {
    CHECK_EQ(paths[0], f);
    CHECK_EQ(paths[1], a);
    CHECK_EQ(paths[2], b);
  }
  CHECK(fld::parse_uri_list(fld::make_uri_list({f, b})) == (std::vector<std::string>{f, b}));

  // drop rules
  CHECK_EQ(fld::drop_action({f}, b, 0), fld::DROP_MOVE);           // same file system
  CHECK_EQ(fld::drop_action({f}, b, FL_CTRL), fld::DROP_COPY);     // Ctrl copies
  CHECK_EQ(fld::drop_action({f}, b, FL_SHIFT), fld::DROP_MOVE);
  CHECK_EQ(fld::drop_action({f}, a, 0), fld::DROP_NONE);           // already there
  CHECK_EQ(fld::drop_action({f}, a, FL_CTRL), fld::DROP_COPY);     // duplicate
  CHECK_EQ(fld::drop_action({a}, a, 0), fld::DROP_NONE);           // onto itself
  CHECK_EQ(fld::drop_action({root}, b, FL_CTRL), fld::DROP_NONE);  // into itself
  CHECK_EQ(fld::drop_action({f}, "trash:///", 0), fld::DROP_TRASH);
  CHECK_EQ(fld::drop_action({}, b, 0), fld::DROP_NONE);
  // another file system copies (when /dev/shm is a separate tmpfs)
  struct stat s1, s2;
  if (stat("/dev/shm", &s1) == 0 && stat(root.c_str(), &s2) == 0 && s1.st_dev != s2.st_dev &&
      access("/dev/shm", W_OK) == 0)
    CHECK_EQ(fld::drop_action({f}, "/dev/shm", 0), fld::DROP_COPY);

  fld::remove_recursive(root);
  return test_result("fileops");
}
