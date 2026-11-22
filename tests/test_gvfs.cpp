// GVfs helpers: URIs, names, fuse paths
#include "gvclient.h"
#include "test.h"

int main() {
  // locations
  CHECK(gv::looks_like_uri("smb://server/share"));
  CHECK(gv::looks_like_uri("sftp://user@host:2222/home"));
  CHECK(gv::looks_like_uri("davs://example.org/dav/"));
  CHECK(gv::looks_like_uri("network:///"));
  CHECK(!gv::looks_like_uri("file:///home"));
  CHECK(!gv::looks_like_uri("trash:///"));
  CHECK(!gv::looks_like_uri("/home/user"));
  CHECK(!gv::looks_like_uri("~/Documents"));
  CHECK(!gv::looks_like_uri("notes:today"));
  CHECK_EQ(gv::uri_scheme("SFTP://host"), std::string("sftp"));
  CHECK_EQ(gv::uri_scheme("/a:b"), std::string(""));

  // names
  CHECK_EQ(gv::display_name_for_uri("smb://server/share"), std::string("share on server"));
  CHECK_EQ(gv::display_name_for_uri("smb://server/share/"), std::string("share on server"));
  CHECK_EQ(gv::display_name_for_uri("sftp://alice@host:2222/"), std::string("alice@host"));
  CHECK_EQ(gv::display_name_for_uri("sftp://host/srv/My%20Files"), std::string("My Files on host"));
  CHECK_EQ(gv::display_name_for_uri("smb://WORK;bob@nas/music"), std::string("music on nas"));
  CHECK_EQ(gv::display_name_for_uri("ftp://ftp.example.org"), std::string("ftp.example.org"));
  CHECK_EQ(gv::display_name_for_uri("davs://[::1]:8443/dav"), std::string("dav on [::1]"));

  // fuse paths and their URIs
  const std::string root = "/run/user/1000/gvfs";
  CHECK(gv::is_fuse_path(root + "/smb-share:server=nas,share=music", root));
  CHECK(gv::is_fuse_path(root + "/sftp:host=h/home", root));
  CHECK(!gv::is_fuse_path(root, root));
  CHECK(!gv::is_fuse_path("/run/user/1000/gvfsx/a", root));
  gv::State st;
  gv::Mount m;
  m.uri = "smb://nas/music/";
  m.path = root + "/smb-share:server=nas,share=music";
  st.mounts.push_back(m);
  gv::Mount other;
  other.uri = "sftp://h/";
  other.path = root + "/sftp:host=h";
  st.mounts.push_back(other);
  CHECK(st.mount_for_path(m.path + "/Rock/a b.mp3") == &st.mounts[0]);
  CHECK(st.mount_for_path(root + "/smb-share:server=nas,share=musicx") == nullptr);
  CHECK_EQ(gv::uri_for_path(st, m.path), std::string("smb://nas/music/"));
  CHECK_EQ(gv::uri_for_path(st, m.path + "/Rock/a b.mp3"), std::string("smb://nas/music/Rock/a%20b.mp3"));
  CHECK_EQ(gv::uri_for_path(st, "/home/user"), std::string(""));
  return test_result("gvfs");
}
