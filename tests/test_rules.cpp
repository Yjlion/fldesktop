// fl-wm: window rules ([rule:<name>] in wm.ini)
extern "C" {
#include "rules.h"
}
#include <cstring>
#include "test.h"

int main() {
  struct flwm_rule r[3];
  for (auto &x : r) rule_init(&x);
  CHECK(rule_set(&r[0], "app_id", "fl-term"));
  CHECK(rule_set(&r[0], "workspace", "2"));
  CHECK(rule_set(&r[0], "size", "900x600"));
  CHECK(rule_set(&r[0], "position", "center"));
  CHECK(rule_set(&r[0], "always_on_top", "yes"));
  CHECK(!rule_set(&r[0], "workspace", "0"));
  CHECK(!rule_set(&r[0], "size", "10x10"));
  CHECK(!rule_set(&r[0], "opacity", "2"));
  CHECK(!rule_set(&r[0], "opacity", "0.05"));
  CHECK(!rule_set(&r[0], "maximized", "maybe"));
  CHECK(!rule_set(&r[0], "colour", "red"));
  CHECK(!rule_set(&r[0], "position", "10;20"));
  CHECK_EQ(r[0].workspace, 2);  // the bad values left it as it was
  CHECK(rule_set(&r[1], "title", "*htop*"));
  CHECK(rule_set(&r[1], "opacity", "0.8"));
  CHECK(rule_set(&r[1], "workspace", "3"));
  CHECK(rule_set(&r[1], "position", "10,20"));
  // no app_id and no title: matches nothing
  CHECK(rule_set(&r[2], "maximized", "true"));

  CHECK(rule_matches(&r[0], "fl-term", "anything"));
  CHECK(rule_matches(&r[0], "FL-TERM", nullptr)); // case does not matter
  CHECK(!rule_matches(&r[0], "fl-files", "x"));
  CHECK(!rule_matches(&r[0], nullptr, "x"));
  CHECK(rule_matches(&r[1], "fl-term", "user@host: htop"));
  CHECK(!rule_matches(&r[2], "fl-term", "x"));

  struct flwm_rule_result res;
  rules_apply(r, 3, "fl-term", "htop - 4 tasks", &res);
  CHECK_EQ(res.matched, 2);
  CHECK_EQ(res.workspace, 3); // the later rule wins
  CHECK_EQ(res.width, 900);
  CHECK_EQ(res.height, 600);
  CHECK(res.has_position && !res.center);
  CHECK_EQ(res.x, 10);
  CHECK_EQ(res.y, 20);
  CHECK_EQ((int)res.above, 1);
  CHECK_EQ((int)res.maximized, -1); // not set by a matching rule
  CHECK(res.opacity > 0.79f && res.opacity < 0.81f);
  rules_apply(r, 3, "fl-files", "Home", &res);
  CHECK_EQ(res.matched, 0);
  CHECK_EQ((int)res.sticky, -1);
  for (auto &x : r) rule_free(&x);
  CHECK(r[0].app_id == nullptr);
  return test_result("rules");
}
