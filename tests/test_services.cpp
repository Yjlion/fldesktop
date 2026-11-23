// fl-services: systemctl and OpenRC output
#include "services_util.h"
#include "test.h"

using namespace svc;

int main() {
  auto v = parse_systemctl_units(
      "sshd.service            loaded    active   running OpenSSH Daemon\n"
      "\xe2\x97\x8f cups.service  loaded    failed   failed  CUPS Scheduler\n"
      "systemd-tmpfiles-setup.service loaded active exited Create Volatile Files\n"
      "ghost.service           not-found inactive dead    ghost.service\n"
      "foo.socket              loaded    active   listening Foo Socket\n");
  CHECK_EQ(v.size(), (size_t)3);
  CHECK_EQ(v[0].name, std::string("sshd"));
  CHECK_EQ(v[0].description, std::string("OpenSSH Daemon"));
  CHECK(v[0].running());
  CHECK_EQ(state_label(v[0]), std::string("Running"));
  CHECK(v[1].failed());
  CHECK_EQ(state_label(v[1]), std::string("Failed"));
  CHECK(!v[2].running());
  CHECK_EQ(state_label(v[2]), std::string("Done"));
  merge_unit_files(v, "sshd.service enabled disabled\ncups.service disabled enabled\n"
                      "bluetooth.service enabled enabled\ngetty@.service enabled enabled\n"
                      "systemd-tmpfiles-setup.service static -\n");
  CHECK_EQ(v.size(), (size_t)4); // bluetooth added, the template left out
  CHECK_EQ(v[0].name, std::string("bluetooth"));
  CHECK_EQ(v[0].enabled, std::string("enabled"));
  CHECK_EQ(state_label(v[0]), std::string("Stopped"));
  CHECK_EQ(v[1].enabled, std::string("disabled")); // cups
  CHECK_EQ(v[2].enabled, std::string("enabled"));  // sshd

  auto r = parse_rc_status("Runlevel: default\n sshd                [  started  ]\n local      [  stopped  ]\n"
                           "Dynamic Runlevel: hotplugged\nRunlevel: boot\n hwclock [ started ]\n"
                           " crond [ crashed ]\n supervised [ started 00:01:02 (0) ]\n");
  CHECK_EQ(r.size(), (size_t)5);
  CHECK_EQ(r[0].name, std::string("crond"));
  CHECK(r[0].failed());
  CHECK_EQ(r[1].name, std::string("hwclock"));
  CHECK(r[1].running());
  CHECK_EQ(r[3].name, std::string("sshd"));
  CHECK_EQ(r[4].name, std::string("supervised"));
  CHECK_EQ(r[4].active, std::string("started"));
  merge_rc_update(r, "          sshd | boot default\n           local |\n       nginx |\n");
  CHECK_EQ(r.size(), (size_t)6);
  for (auto &s : r) {
    if (s.name == "sshd") CHECK_EQ(s.enabled, std::string("boot default"));
    if (s.name == "local") CHECK_EQ(s.enabled, std::string(""));
    if (s.name == "nginx") CHECK_EQ(s.active, std::string("stopped"));
  }

  CHECK(valid_name("sshd"));
  CHECK(valid_name("getty@tty1"));
  CHECK(valid_name("systemd-fsck@dev-disk-by\\x2duuid-1234"));
  CHECK(!valid_name("../bin/sh"));
  CHECK(!valid_name("a b"));
  CHECK(!valid_name("-rf"));
  CHECK(!valid_name(""));
  return test_result("services");
}
