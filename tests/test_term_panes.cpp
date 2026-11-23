// fl-term: split panes, scrollback search, profiles
#include "fld/ini.h"
#include "term_panes.h"
#include "term_settings.h"
#include "test.h"

using namespace term;

int main() {
  PaneTree t(1);
  CHECK_EQ(t.describe(), std::string("1"));
  CHECK(t.split(1, 2, true));
  CHECK(!t.split(9, 3, true)); // no such pane
  CHECK(!t.split(1, 2, true)); // 2 is there already
  CHECK(t.split(2, 3, false));
  CHECK_EQ(t.describe(), std::string("(h 1 (v 2 3))"));
  CHECK_EQ(t.count(), (size_t)3);
  CHECK(t.ids() == std::vector<PaneTree::Id>({1, 2, 3}));

  Rect r{0, 0, 204, 104};
  auto l = t.layout(r, 4);
  CHECK_EQ(l.size(), (size_t)3);
  CHECK_EQ(l[0].second.w, 100);
  CHECK_EQ(l[1].second.x, 104);
  CHECK_EQ(l[1].second.h, 50);
  CHECK_EQ(l[2].second.y, 54);
  CHECK_EQ(t.neighbor(1, Dir::Right, r, 4), (PaneTree::Id)2);
  CHECK_EQ(t.neighbor(3, Dir::Up, r, 4), (PaneTree::Id)2);
  CHECK_EQ(t.neighbor(3, Dir::Left, r, 4), (PaneTree::Id)1);
  CHECK_EQ(t.neighbor(1, Dir::Left, r, 4), (PaneTree::Id)0);

  auto dv = t.dividers(r, 4);
  CHECK_EQ(dv.size(), (size_t)2);
  CHECK_EQ(dv[0].x, 100);
  CHECK_EQ(dv[0].w, 4);
  CHECK_EQ(dv[1].y, 50);
  CHECK_EQ(dv[1].x, 104);
  bool vbar = false;
  CHECK(t.divider_at(r, 4, 102, 10, &vbar));
  CHECK(vbar);
  CHECK(t.divider_at(r, 4, 150, 52, &vbar));
  CHECK(!vbar);
  CHECK(!t.divider_at(r, 4, 50, 50, nullptr));
  CHECK(t.drag_begin(r, 4, 102, 10));
  t.drag_to(51, 10);
  t.drag_end();
  CHECK_EQ(t.layout(r, 4)[0].second.w, 50);
  t.drag_to(0, 0); // not dragging: nothing
  CHECK(!t.drag_begin(r, 4, 102, 10)); // the divider has moved
  CHECK(t.drag_begin(r, 4, 52, 10));
  t.drag_to(0, 0); // limited to 10%
  CHECK_EQ(t.layout(r, 4)[0].second.w, 20);
  t.drag_end();

  CHECK(t.remove(2));
  CHECK_EQ(t.describe(), std::string("(h 1 3)"));
  CHECK(t.remove(1));
  CHECK_EQ(t.describe(), std::string("3"));
  CHECK(!t.remove(3)); // the last one stays
  CHECK(t.contains(3));

  // search
  std::vector<std::u32string> lines = {to_u32("make all"), to_u32("error: Foo"), to_u32("ok"), to_u32("ERROR: bar")};
  Match m = find_in_lines(lines, to_u32("error"), 3, 99, true, false);
  CHECK_EQ(m.line, 1L);
  CHECK_EQ(m.col, 0);
  m = find_in_lines(lines, to_u32("error"), 3, 99, true, true);
  CHECK_EQ(m.line, 3L);
  m = find_in_lines(lines, to_u32("error"), 3, 0, true, true);
  CHECK_EQ(m.line, 1L);
  m = find_in_lines(lines, to_u32("error"), 1, 0, true, false); // wraps around to itself
  CHECK_EQ(m.line, 1L);
  m = find_in_lines(lines, to_u32("o"), 1, 7, false, false);
  CHECK_EQ(m.line, 1L);
  CHECK_EQ(m.col, 8);
  m = find_in_lines(lines, to_u32("o"), 1, 8, false, false);
  CHECK_EQ(m.line, 1L);
  CHECK_EQ(m.col, 9);
  m = find_in_lines(lines, to_u32("o"), 1, 9, false, false);
  CHECK_EQ(m.line, 2L);
  CHECK(find_in_lines(lines, to_u32("zzz"), 0, 0, false, true).line < 0);
  CHECK(find_in_lines({}, to_u32("a"), 0, 0, false, true).line < 0);
  CHECK(to_u32("ä€") == std::u32string({0xE4, 0x20AC}));

  // profiles
  fld::Ini ini;
  ini.parse("[Terminal]\nDefaultProfile=Work\n[Profile Work]\nCommand=tmux\nColorScheme=light\nFontSize=16\n"
            "Opacity=90\n[Profile Root]\nCommand=sudo -i\n[ProfileX]\nCommand=x\n");
  auto ps = load_profiles(ini);
  CHECK_EQ(ps.size(), (size_t)2);
  const Profile *w = find_profile(ps, "Work");
  CHECK(w != nullptr);
  CHECK_EQ(w->command, std::string("tmux"));
  CHECK_EQ(w->font_size, 16);
  CHECK_NEAR(w->opacity, 0.9);
  CHECK(find_profile(ps, "nope") == nullptr);
  Settings s;
  load(ini, s);
  CHECK_EQ(s.default_profile, std::string("Work"));
  ps.pop_back();
  store_profiles(ps, ini);
  auto again = load_profiles(ini);
  CHECK_EQ(again.size(), (size_t)1);
  CHECK_EQ(again[0].scheme, std::string("light"));
  return test_result("term-panes");
}
