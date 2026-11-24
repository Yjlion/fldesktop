// pinentry's Assuan protocol: escaping, the SET commands, GETPIN replies
#include <string>
#include <vector>

#include "assuan.h"
#include "test.h"

int main() {
  using V = std::vector<std::string>;
  CHECK_EQ(assuan::unescape("Enter%0Athe%25 pass%3a"), std::string("Enter\nthe% pass:"));
  CHECK_EQ(assuan::escape("a%b\nc"), std::string("a%25b%0Ac"));
  CHECK_EQ(assuan::unescape("50%"), std::string("50%"));

  assuan::Request seen;
  std::string pin = "s3cret%\n";
  assuan::Ui ui;
  ui.get_pin = [&](const assuan::Request &r) {
    seen = r;
    assuan::Answer a;
    a.kind = pin.empty() ? assuan::Answer::Cancel : assuan::Answer::Ok;
    a.pin = pin;
    a.repeated = !r.repeat.empty();
    return a;
  };
  ui.confirm = [&](const assuan::Request &r) {
    seen = r;
    assuan::Answer a;
    a.kind = r.notok.empty() ? assuan::Answer::Ok : assuan::Answer::NotOk;
    return a;
  };
  assuan::Session s(ui);
  CHECK(s.handle("SETDESC Please enter the passphrase%0Afor key 0x1234") == V({"OK"}));
  CHECK(s.handle("SETPROMPT Passphrase:") == V({"OK"}));
  CHECK(s.handle("SETREPEAT") == V({"OK"}));
  CHECK(s.handle("OPTION allow-external-password-cache") == V({"OK"}));
  CHECK(s.handle("OPTION ttytype=xterm") == V({"OK"}));
  CHECK(s.handle("SETKEYINFO s/ABCDEF") == V({"OK"}));
  CHECK(s.handle("GETPIN\n") == V({"S PIN_REPEATED", "D s3cret%25%0A", "OK"}));
  CHECK_EQ(seen.description, std::string("Please enter the passphrase\nfor key 0x1234"));
  CHECK(seen.allow_external_cache);
  CHECK_EQ(seen.keyinfo, std::string("s/ABCDEF"));
  CHECK_EQ(s.request().options.at("ttytype"), std::string("xterm"));
  pin.clear();
  auto r = s.handle("GETPIN");
  CHECK(r.size() == 1 && r[0].compare(0, 13, "ERR 83886179 ") == 0);
  CHECK(s.handle("CONFIRM") == V({"OK"}));
  CHECK(s.handle("SETNOTOK _No") == V({"OK"}));
  r = s.handle("CONFIRM");
  CHECK(r.size() == 1 && r[0].compare(0, 13, "ERR 83886194 ") == 0);
  CHECK(s.handle("GETINFO flavor") == V({"D fldesktop", "OK"}));
  r = s.handle("FROBNICATE");
  CHECK(r.size() == 1 && r[0].compare(0, 4, "ERR ") == 0);
  CHECK(s.handle("RESET") == V({"OK"}));
  CHECK(s.request().description.empty());
  CHECK(!s.finished());
  s.handle("BYE");
  CHECK(s.finished());
  CHECK(assuan::quality("") == 0);
  CHECK(assuan::quality("aaaa") < assuan::quality("correct horse battery staple"));
  CHECK(assuan::quality("Tr0ub4dor&3xYz!Q9#mPl") == 100);
  return test_result("pinentry");
}
