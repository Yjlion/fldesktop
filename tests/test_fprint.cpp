// fld-fprint: the helpers; with FLD_FPRINT_MOCK, checking and enrolling
// against tests/tools/fake-fprintd.py (fprint-mock.sh)
#include <FL/Fl.H>

#include <chrono>
#include <cstdlib>

#include "fprint.h"
#include "test.h"

static void wait_for(bool &flag, double secs) {
  auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(secs * 1000));
  while (!flag && std::chrono::steady_clock::now() < end) Fl::wait(0.05);
}

int main() {
  Fl::lock();
  CHECK_EQ(fprint::finger_label("right-index-finger"), std::string("Right index finger"));
  CHECK_EQ(fprint::all_fingers().size(), (size_t)10);
  CHECK_EQ(fprint::status_text("verify-no-match").compare(0, 3, "Not"), 0);
  CHECK_EQ(fprint::status_text("enroll-completed"), std::string("Done"));
  if (getenv("FLD_FPRINT_MOCK")) {
    CHECK(fprint::available());
    auto f = fprint::enrolled("me");
    CHECK(f.size() == 1 && f[0] == "right-index-finger");
    bool ready = false, nomatch = false, match = false;
    {
      fprint::Verify v("me", [&](fprint::Verify::Event e, const std::string &) {
        if (e == fprint::Verify::READY) ready = true;
        if (e == fprint::Verify::NO_MATCH) nomatch = true;
        if (e == fprint::Verify::MATCH) match = true;
      });
      wait_for(match, 5);
    }
    CHECK(ready);
    CHECK(nomatch); // the first try, then a new scan
    CHECK(match);
    int last_stage = 0, stages = 0;
    bool done = false, ok = false;
    {
      fprint::Enroll en("me", "left-thumb", [&](int st, int n, const std::string &, bool d, bool o) {
        last_stage = st;
        stages = n;
        if (d) done = true, ok = o;
      });
      wait_for(done, 5);
    }
    CHECK(done && ok);
    CHECK_EQ(stages, 3);
    CHECK_EQ(last_stage, 3);
    std::string err;
    CHECK(fprint::delete_all("me", err));
  } else {
    // no fprintd on a bus that has none
    setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent", 1);
    CHECK(!fprint::available());
    CHECK(fprint::enrolled("me").empty());
  }
  return test_result("fprint");
}
