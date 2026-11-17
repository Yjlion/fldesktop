#include <cstdlib>
#include <unistd.h>

#include "fld/trash.h"
#include "fld/util.h"
#include "ssh_profiles.h"
#include "test.h"

using namespace ssh;

static std::string join(const std::vector<std::string> &v) { return fld::join(v, " "); }

int main() {
  // ---- targets
  Profile t = parse_target("alice@example.org:2222");
  CHECK_EQ(t.user, std::string("alice"));
  CHECK_EQ(t.host, std::string("example.org"));
  CHECK_EQ(t.port, 2222);
  CHECK_EQ(t.name, std::string("alice@example.org"));
  t = parse_target("ssh://bob@[2001:db8::1]:22/");
  CHECK_EQ(t.user, std::string("bob"));
  CHECK_EQ(t.host, std::string("2001:db8::1"));
  CHECK_EQ(t.port, 22);
  t = parse_target("2001:db8::2");
  CHECK_EQ(t.host, std::string("2001:db8::2"));
  CHECK_EQ(t.port, 0);
  t = parse_target("  plainhost ");
  CHECK_EQ(t.target(), std::string("plainhost"));

  // ---- argv
  Profile p;
  p.host = "srv";
  p.user = "me";
  p.port = 2200;
  p.identity = "/k/id";
  p.jump = "gw";
  p.forwards = {"L 8080:localhost:80", " d 1080", "x bogus", "R 9000:localhost:9000"};
  p.options = "-C -o 'ServerAliveInterval 30'";
  CHECK_EQ(join(build_argv(p)), std::string("ssh -p 2200 -i /k/id -J gw -L 8080:localhost:80 -D 1080 -R "
                                            "9000:localhost:9000 -C -o ServerAliveInterval 30 me@srv"));
  CHECK_EQ(build_argv(p)[15], std::string("ServerAliveInterval 30"));
  p = Profile();
  p.host = "h";
  p.command = "tmux new -A -s main";
  auto a = build_argv(p);
  CHECK_EQ(join(a), std::string("ssh -t h tmux new -A -s main"));
  CHECK_EQ(a.back(), std::string("tmux new -A -s main")); // one argument for the remote shell

  // ---- ssh_config
  char tmpl[] = "/tmp/fld-ssh-XXXXXX";
  std::string dir = mkdtemp(tmpl);
  fld::mkdir_p(dir + "/conf.d");
  fld::write_file(dir + "/config",
                  "# comment\nHost work web-*\n  HostName 10.0.0.5\n  User admin\nHost *\n  ServerAliveInterval 30\n"
                  "Include conf.d/*.conf\nHost=\"quoted\"\n  Port=2022\nMatch host x\nHost !neg pi\n");
  fld::write_file(dir + "/conf.d/a.conf", "Host extra\nHost work\n");
  auto hosts = read_ssh_config(dir + "/config", dir);
  std::vector<std::string> names;
  for (auto &h : hosts) names.push_back(h.name);
  CHECK_EQ(join(names), std::string("work extra quoted pi"));
  CHECK(hosts[0].from_ssh_config);
  CHECK_EQ(hosts[0].target(), std::string("work")); // ssh applies User/HostName itself
  CHECK(read_ssh_config(dir + "/missing", dir).empty());

  // ---- saved profiles
  std::string ini = dir + "/ssh.ini";
  Profile s1;
  s1.name = "Web server";
  s1.host = "web.example";
  s1.user = "deploy";
  s1.port = 2201;
  s1.forwards = {"L 1:a:1", "D 9"};
  s1.command = "htop";
  s1.scheme = "red";
  Profile s2;
  s2.name = "work"; // overrides the ssh_config entry
  s2.host = "work";
  s2.user = "me";
  Profile skip = hosts[1];
  CHECK(save_profiles(ini, {s1, s2, skip}));
  auto loaded = load_profiles(ini);
  CHECK_EQ(loaded.size(), (size_t)2);
  const Profile *w = find(loaded, "Web server");
  CHECK(w != nullptr);
  if (w) {
    CHECK_EQ(w->host, std::string("web.example"));
    CHECK_EQ(w->port, 2201);
    CHECK_EQ(w->forwards.size(), (size_t)2);
    CHECK_EQ(w->command, std::string("htop"));
    CHECK_EQ(w->scheme, std::string("red"));
  }
  auto all = all_profiles(ini, dir + "/config", dir);
  names.clear();
  for (auto &h : all) names.push_back(h.name);
  CHECK_EQ(join(names), std::string("Web server work extra quoted pi"));
  CHECK_EQ(find(all, "work")->user, std::string("me"));
  CHECK(find(all, "nope") == nullptr);
  // saving again replaces the old sections
  CHECK(save_profiles(ini, {s2}));
  CHECK_EQ(load_profiles(ini).size(), (size_t)1);

  fld::remove_recursive(dir);
  return test_result("ssh");
}
