// PackageKit helpers: package ids, history lines, texts
#include <packagekit-glib2/packagekit.h>

#include "pkclient.h"
#include "test.h"

int main() {
  pk::Package p = pk::parse_id("nano;9.3-1;x86_64;installed:core");
  CHECK_EQ(p.name, std::string("nano"));
  CHECK_EQ(p.version, std::string("9.3-1"));
  CHECK_EQ(p.arch, std::string("x86_64"));
  CHECK_EQ(p.repo, std::string("core"));
  p = pk::parse_id("libc6;2.41-12;amd64;debian-security");
  CHECK_EQ(p.repo, std::string("debian-security"));
  p.info = PK_INFO_ENUM_INSTALLED;
  CHECK(p.installed());
  CHECK(!p.is_update());
  p.info = PK_INFO_ENUM_SECURITY;
  CHECK(p.is_update());
  CHECK_EQ(pk::info_text(PK_INFO_ENUM_SECURITY), std::string("Security update"));
  CHECK_EQ(pk::status_text(PK_STATUS_ENUM_DOWNLOAD), std::string("Downloading packages"));
  CHECK_EQ(pk::role_text("remove-packages"), std::string("Removed"));
  auto lines = pk::history_lines("installing\tnano;9.3-1;x86_64;core\nremoving\tvim;9.1-1;x86_64;extra\nbad line");
  CHECK_EQ(lines.size(), (size_t)2);
  CHECK_EQ(lines[0], std::string("Installing nano 9.3-1"));
  CHECK_EQ(lines[1], std::string("Removing vim 9.1-1"));
  CHECK_EQ(pk::group_text("programming"), std::string("Development"));
  // every offered group is one PackageKit knows
  for (auto &g : pk::groups()) CHECK(pk_group_enum_from_string(g.c_str()) != PK_GROUP_ENUM_UNKNOWN);
  pk::Error e;
  CHECK(!e);
  e.code = PK_ERROR_ENUM_MISSING_GPG_SIGNATURE;
  e.text = "x";
  CHECK(e);
  CHECK(e.untrusted());
  return test_result("packages");
}
