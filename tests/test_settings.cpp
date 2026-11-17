#include "locale_env.h"
#include "test.h"
#include "xkb_rules.h"

int main() {
  // base.lst fixture
  xkb::Rules r;
  CHECK(r.parse("! model\n"
                "  pc105           Generic 105-key PC\n"
                "\n"
                "! layout\n"
                "  us              English (US)\n"
                "  de              German\n"
                "\n"
                "! variant\n"
                "  intl            us: English (US, intl., with dead keys)\n"
                "  nodeadkeys      de: German (no dead keys)\n"
                "\n"
                "! option\n"
                "  grp             Switching to another layout\n"
                "  grp:alt_shift_toggle Alt+Shift\n"
                "  ctrl:nocaps     Caps Lock as Ctrl\n"));
  CHECK_EQ(r.models.size(), 1u);
  CHECK_EQ(r.layouts.size(), 2u);
  CHECK_EQ(r.layouts[1].name, std::string("de"));
  CHECK_EQ(r.layouts[1].description, std::string("German"));
  CHECK_EQ(r.variants["de"].size(), 1u);
  CHECK_EQ(r.variants["de"][0].name, std::string("nodeadkeys"));
  CHECK_EQ(r.variants["de"][0].description, std::string("German (no dead keys)"));
  CHECK_EQ(r.layout_description("us"), std::string("English (US)"));
  CHECK_EQ(r.layout_description("de", "nodeadkeys"), std::string("German (no dead keys)"));
  auto grp = r.options_of("grp");
  CHECK_EQ(grp.size(), 1u);
  CHECK_EQ(grp[0].name, std::string("grp:alt_shift_toggle"));

  auto lv = xkb::split_layouts("us, de", ",nodeadkeys");
  CHECK_EQ(lv.size(), 2u);
  CHECK_EQ(lv[1].second, std::string("nodeadkeys"));
  std::string l, v;
  xkb::join_layouts(lv, l, v);
  CHECK_EQ(l, std::string("us,de"));
  CHECK_EQ(v, std::string(",nodeadkeys"));
  xkb::join_layouts({{"us", ""}, {"fr", ""}}, l, v);
  CHECK_EQ(l, std::string("us,fr"));
  CHECK_EQ(v, std::string(""));

  // environment file writer
  locale_env::EnvFile env;
  env.parse("# my settings\nexport EDITOR=vim\nLANG=\"en_US.UTF-8\"\nLC_TIME=C\n");
  CHECK_EQ(env.get("LANG"), std::string("en_US.UTF-8"));
  CHECK_EQ(env.get("EDITOR"), std::string("vim"));
  env.set("LANG", "de_DE.UTF-8");
  env.set("LC_TIME", "");
  env.set("LC_PAPER", "de_DE.UTF-8");
  CHECK_EQ(env.text(), std::string("# my settings\nexport EDITOR=vim\nLANG=de_DE.UTF-8\nLC_PAPER=de_DE.UTF-8\n"));

  CHECK_EQ(locale_env::normalize("de_DE.utf8"), std::string("de_DE.UTF-8"));
  CHECK_EQ(locale_env::normalize("C.UTF-8"), std::string("C.UTF-8"));
  return test_result("settings");
}
