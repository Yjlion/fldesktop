// Bluetooth helpers, and (with FLD_BT_MOCK=1, run by bt-mock.sh against
// python-dbusmock's bluez5 template) the client against a mock BlueZ.
#include <FL/Fl.H>

#include <cstdlib>
#include <cstring>
#include <functional>

#include "btclient.h"
#include "fld/util.h"
#include "test.h"

static bool wait_for(std::function<bool()> cond, double secs = 5) {
  for (double t = 0; t < secs; t += 0.02) {
    if (cond()) return true;
    Fl::wait(0.02);
  }
  return cond();
}

int main() {
  using namespace bt;
  CHECK_EQ(format_passkey(42), std::string("000042"));
  CHECK_EQ(format_passkey(123456), std::string("123456"));
  CHECK_EQ(std::string(device_icon("audio-headset", 0)), std::string("audio-headset"));
  CHECK_EQ(std::string(device_icon("", 0x240404)), std::string("audio-headset")); // major class 4 = audio
  CHECK_EQ(std::string(device_icon("", 0x5a020c)), std::string("phone"));
  CHECK_EQ(std::string(device_icon("weird", 0)), std::string("bluetooth"));
  CHECK_EQ(error_text("org.bluez.Error.AuthenticationFailed", "x"),
           std::string("Authentication failed. Check the PIN or passkey."));
  CHECK_EQ(error_text("org.example.Other", "Some message"), std::string("Some message"));
  // rfkill from a fake sysfs tree
  std::string root = fld::path_join(getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", "fld-rfkill-test");
  fld::mkdir_p(root + "/rfkill0");
  fld::mkdir_p(root + "/rfkill1");
  fld::write_file(root + "/rfkill0/type", "wlan\n");
  fld::write_file(root + "/rfkill0/soft", "1\n");
  fld::write_file(root + "/rfkill1/type", "bluetooth\n");
  fld::write_file(root + "/rfkill1/soft", "0\n");
  fld::write_file(root + "/rfkill1/hard", "1\n");
  bool soft, hard;
  rfkill_state(root, soft, hard);
  CHECK(!soft);
  CHECK(hard);
  State st;
  Device d;
  d.connected = true;
  st.devices = {d, Device()};
  CHECK_EQ(st.connected_count(), 1);

  if (getenv("FLD_BT_MOCK")) {
    Fl::lock();
    Client &c = Client::get();
    c.start();
    CHECK(wait_for([&] { return c.state().running && c.state().adapter() && c.state().devices.size() == 2; }));
    const Adapter *a = c.state().adapter();
    CHECK(a && a->powered);
    std::string dev;
    for (auto &x : c.state().devices)
      if (!x.paired) dev = x.path;
    CHECK(!dev.empty());
    std::string err = "pending";
    c.pair(dev, [&](const std::string &e) { err = e; });
    CHECK(wait_for([&] { return err != "pending"; }));
    CHECK_EQ(err, std::string(""));
    CHECK(wait_for([&] {
      const Device *x = c.state().device(dev);
      return x && x->paired && x->connected;
    }));
    bool connected_event = false;
    c.on_connection = [&](const Device &, bool on) { connected_event = !on; };
    err = "pending";
    c.disconnect(dev, [&](const std::string &e) { err = e; });
    CHECK(wait_for([&] { return connected_event; }));
    CHECK_EQ(err, std::string(""));
    err = "pending";
    c.set_powered(false, [&](const std::string &e) { err = e; });
    CHECK(wait_for([&] { return c.state().adapter() && !c.state().adapter()->powered; }));
    err = "pending";
    c.remove(dev, [&](const std::string &e) { err = e; });
    CHECK(wait_for([&] { return !c.state().device(dev); }));
    CHECK_EQ(err, std::string(""));
  }
  return test_result("bt");
}
