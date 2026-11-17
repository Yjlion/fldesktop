// fl-term settings (term.ini)
#include "fld/ini.h"
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
  return test_result("term");
}
