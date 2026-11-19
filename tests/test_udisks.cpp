// UDisks2 helpers
#include "test.h"
#include "udclient.h"

int main() {
  using namespace ud;
  CHECK_EQ(volume_label("My Stick", "LABEL", 0), std::string("My Stick"));
  CHECK_EQ(volume_label("", " DATA ", 0), std::string("DATA"));
  CHECK_EQ(volume_label("", "", 16000000000ULL), std::string("16 GB Volume"));
  CHECK_EQ(volume_label("", "", 2500000000ULL), std::string("2.5 GB Volume"));
  CHECK_EQ(volume_label("", "", 0), std::string("Volume"));
  CHECK(volume_shown("filesystem", false, false, false, ""));
  CHECK(volume_shown("crypto", false, false, false, ""));
  CHECK(!volume_shown("other", false, false, false, ""));          // swap
  CHECK(!volume_shown("filesystem", true, false, false, ""));      // HintIgnore
  CHECK(!volume_shown("filesystem", false, true, false, ""));      // internal, not mounted
  CHECK(!volume_shown("filesystem", false, true, true, "/boot"));  // internal system mount
  CHECK(volume_shown("filesystem", false, true, true, "/run/media/me/Data"));
  CHECK_EQ(error_text("org.freedesktop.UDisks2.Error.DeviceBusy", "x"),
           std::string("The volume is in use. Close the files and programs using it, then try again."));
  CHECK_EQ(error_text("org.freedesktop.UDisks2.Error.NotAuthorizedCanObtain", "x"),
           std::string("You are not allowed to do this."));
  CHECK_EQ(error_text("org.freedesktop.UDisks2.Error.Failed", "Error unlocking: wrong passphrase"),
           std::string("Wrong passphrase."));
  CHECK_EQ(error_text("other", "message"), std::string("message"));
  State s;
  Volume v;
  v.path = "/a";
  v.mount_point = "/run/media/me/X";
  s.volumes = {v};
  CHECK(s.volume("/a") && s.by_mount_point("/run/media/me/X"));
  CHECK(!s.volume("/b") && !s.by_mount_point(""));
  return test_result("udisks");
}
