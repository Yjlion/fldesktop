// fl-term settings (term.ini)
#include "fld/ini.h"
#include "term_runs.h"
#include "term_settings.h"
#include "test.h"

int main() {
  using namespace term;
  Settings def;
  CHECK_EQ(def.tabs, TABS_AUTO);
  CHECK(!def.show_menubar);
  CHECK_NEAR(def.opacity, 1.0);

  CHECK_EQ(parse_tabs_mode("always"), TABS_ALWAYS);
  CHECK_EQ(parse_tabs_mode(" Never "), TABS_NEVER);
  CHECK_EQ(parse_tabs_mode("true"), TABS_ALWAYS);
  CHECK_EQ(parse_tabs_mode("auto"), TABS_AUTO);
  CHECK_EQ(parse_tabs_mode("bogus"), TABS_AUTO);
  CHECK(!tabs_visible(TABS_AUTO, 1));
  CHECK(tabs_visible(TABS_AUTO, 2));
  CHECK(tabs_visible(TABS_ALWAYS, 1));
  CHECK(!tabs_visible(TABS_NEVER, 5));
  CHECK_NEAR(clamp_opacity(0.0), 0.2);
  CHECK_NEAR(clamp_opacity(3.0), 1.0);

  fld::Ini ini;
  ini.parse("[Terminal]\nColorScheme=light\nFontSize=12\nOpacity=80\nShowMenuBar=true\nShowTabs=never\n");
  Settings s;
  load(ini, s);
  CHECK_EQ(s.scheme, std::string("light"));
  CHECK_EQ(s.font_size, 12);
  CHECK_NEAR(s.opacity, 0.8); // a percentage is accepted
  CHECK(s.show_menubar);
  CHECK_EQ(s.tabs, TABS_NEVER);
  CHECK_EQ(s.scrollback, 10000);

  // round trip
  s.opacity = 0.55;
  s.tabs = TABS_ALWAYS;
  fld::Ini out;
  store(s, out);
  Settings back;
  load(out, back);
  CHECK_NEAR(back.opacity, 0.55);
  CHECK_EQ(back.tabs, TABS_ALWAYS);
  CHECK_EQ(back.scheme, std::string("light"));
  CHECK(back.show_menubar);

  // splitting rows into runs drawn with one text call
  {
    auto row = [](const char *s, uint32_t key = 1) {
      std::vector<RunCell> v;
      for (const char *p = s; *p; p++) {
        RunCell c;
        c.ch = *p == '_' ? 0 : (unsigned char)*p; // '_' stands for an empty cell
        c.key = key;
        v.push_back(c);
      }
      return v;
    };
    auto r = split_runs(row("ab  cd   "));
    CHECK_EQ(r.size(), (size_t)1);
    CHECK_EQ(r[0].first, 0);
    CHECK_EQ(r[0].last, 5); // trailing blanks are not part of it
    CHECK(!r[0].single);
    CHECK(split_runs(row("  __  ")).empty());
    r = split_runs(row("_x_"));
    CHECK_EQ(r.size(), (size_t)1);
    CHECK_EQ(r[0].first, 1);
    CHECK_EQ(r[0].last, 1);
    // another colour starts a new run; the blanks between belong to neither
    auto v = row("ab  cd");
    v[4].key = v[5].key = 2;
    r = split_runs(v);
    CHECK_EQ(r.size(), (size_t)2);
    CHECK_EQ(r[0].last, 1);
    CHECK_EQ(r[1].first, 4);
    CHECK_EQ(r[1].last, 5);
    // non-ASCII, wide and lone cells are drawn by themselves
    v = row("abcde");
    v[1].ch = 0xe9;  // é
    v[3].alone = true; // e.g. the cursor
    r = split_runs(v);
    CHECK_EQ(r.size(), (size_t)5);
    CHECK(!r[0].single);
    CHECK(r[1].single);
    CHECK_EQ(r[1].first, 1);
    CHECK(!r[2].single);
    CHECK_EQ(r[2].first, 2);
    CHECK(r[3].single);
    CHECK_EQ(r[3].first, 3);
    CHECK_EQ(r[4].first, 4); // the cursor ended the run before it
    CHECK(!r[4].single);
    v = row("ab");
    v[0].ch = 0x4e2d; // 中, two columns
    v[0].width = 2;
    r = split_runs(v);
    CHECK_EQ(r.size(), (size_t)2);
    CHECK(r[0].single);
    CHECK(!r[1].single);
  }
  return test_result("term");
}
