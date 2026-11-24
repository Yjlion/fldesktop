// libfld's AT-SPI model: names, states, hit testing and events
#include "atspi_model.h"
#include "test.h"

using namespace fld::atspi;

int main() {
  CHECK_EQ(clean_label("&Open…"), std::string("Open…"));
  CHECK_EQ(clean_label("Save && Quit"), std::string("Save & Quit"));
  CHECK_EQ(clean_label("@-> Next"), std::string("Next"));
  CHECK_EQ(clean_browser_line("@b@C8Personal"), std::string("Personal"));
  CHECK_EQ(clean_browser_line("@.@b literal"), std::string("@b literal"));
  CHECK_EQ(clean_browser_line("  Tap to click"), std::string("Tap to click"));
  CHECK_EQ(clean_label("a@@b"), std::string("a@b"));
  CHECK_EQ(clean_label("  two\n lines  "), std::string("two lines"));
  CHECK_EQ(accessible_name("", "Zoom in (+)"), std::string("Zoom in (+)"));
  CHECK_EQ(accessible_name("Name:", ""), std::string("Name"));
  CHECK_EQ(accessible_name("DIRECT-0c-HP M110 Las\u2026", "DIRECT-0c-HP M110 LaserJet"),
           std::string("DIRECT-0c-HP M110 LaserJet")); // cut to fit: the whole name
  CHECK_EQ(accessible_name("Wait\u2026", ""), std::string("Wait\u2026"));
  CHECK_EQ(accessible_name("@fileopen", "Open"), std::string("Open")); // a symbol alone: the tooltip
  CHECK_EQ(std::string(role_name(ROLE_PUSH_BUTTON)), std::string("push button"));
  CHECK_EQ(std::string(role_name(999)), std::string("unknown"));
  auto w = state_words(bit(STATE_FOCUSED) | bit(STATE_READ_ONLY));
  CHECK_EQ(w.first, 1u << 12);
  CHECK_EQ(w.second, 1u << (43 - 32));

  Snapshot a;
  a.nodes[0] = Node{};
  a.nodes[0].role = ROLE_APPLICATION;
  a.nodes[0].children = {1};
  Node win;
  win.id = 1;
  win.parent = 0;
  win.role = ROLE_FRAME;
  win.x = 100;
  win.y = 100;
  win.w = 400;
  win.h = 300;
  win.children = {2, 3};
  a.nodes[1] = win;
  Node b1;
  b1.id = 2;
  b1.parent = 1;
  b1.role = ROLE_PUSH_BUTTON;
  b1.name = "OK";
  b1.x = 110;
  b1.y = 110;
  b1.w = 80;
  b1.h = 30;
  a.nodes[2] = b1;
  Node cb = b1;
  cb.id = 3;
  cb.role = ROLE_CHECK_BOX;
  cb.name = "Wrap";
  cb.y = 150;
  a.nodes[3] = cb;
  a.focus = 2;
  a.active = 1;
  CHECK_EQ(a.at_point(120, 120), 2);
  CHECK_EQ(a.at_point(300, 300), 1);
  CHECK_EQ(a.at_point(10, 10), -1);
  CHECK_EQ(a.index_in_parent(3), 1);
  CHECK_EQ(a.index_in_parent(0), -1);

  Snapshot b = a;
  b.focus = 3;
  b.nodes[3].states |= bit(STATE_CHECKED);
  b.nodes[2].name = "Done";
  auto ev = diff(a, b);
  bool lost = false, got = false, focus = false, checked = false, name = false;
  for (auto &e : ev) {
    if (e.node == 2 && e.kind == "state" && e.detail == "focused" && e.value == 0) lost = true;
    if (e.node == 3 && e.kind == "state" && e.detail == "focused" && e.value == 1) got = true;
    if (e.node == 3 && e.kind == "focus") focus = true;
    if (e.node == 3 && e.kind == "state" && e.detail == "checked" && e.value == 1) checked = true;
    if (e.node == 2 && e.kind == "name") name = true;
  }
  CHECK(lost && got && focus && checked && name);
  CHECK(diff(b, b).empty());
  return test_result("atspi");
}
