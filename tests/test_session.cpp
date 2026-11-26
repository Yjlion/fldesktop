// fl-wm: the session file (session.h)
extern "C" {
#include "session.h"
}
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include "test.h"

int main() {
  char tmpl[] = "/tmp/fld-session-XXXXXX";
  std::string t = mkdtemp(tmpl);
  setenv("XDG_STATE_HOME", t.c_str(), 1);
  char path[1024];
  session_path(path, sizeof path);
  CHECK_EQ(std::string(path), t + "/fldesktop/session.ini");
  flwm_session_entry e[3];
  memset(e, 0, sizeof e);
  snprintf(e[0].app_id, sizeof e[0].app_id, "fl-term");
  e[0].workspace = 1; e[0].x = 10; e[0].y = 40; e[0].w = 800; e[0].h = 500;
  snprintf(e[0].output, sizeof e[0].output, "DP-1");
  snprintf(e[1].app_id, sizeof e[1].app_id, "fl-term");
  e[1].maximized = true; e[1].w = 640; e[1].h = 480;
  snprintf(e[2].app_id, sizeof e[2].app_id, "bad\nid"); // left out
  CHECK(session_write(path, e, 3));
  flwm_session_entry r[8];
  int n = session_read(path, r, 8);
  CHECK_EQ(n, 2);
  CHECK_EQ(std::string(r[0].app_id), std::string("fl-term"));
  CHECK_EQ(r[0].workspace, 1);
  CHECK_EQ(r[0].x, 10);
  CHECK_EQ(r[0].h, 500);
  CHECK_EQ(std::string(r[0].output), std::string("DP-1"));
  CHECK(r[1].maximized);
  // each entry is used once
  CHECK(session_take(r, n, "fl-term") == &r[0]);
  CHECK(session_take(r, n, "fl-term") == &r[1]);
  CHECK(session_take(r, n, "fl-term") == nullptr);
  CHECK(session_take(r, n, "other") == nullptr);
  CHECK(session_take(r, n, nullptr) == nullptr);
  CHECK_EQ(session_read("/nonexistent/x.ini", r, 8), 0);
  std::string cmd = "rm -rf " + t;
  if (system(cmd.c_str()) != 0) {}
  return test_result("session");
}
