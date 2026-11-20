#include "fld/ini.h"
#include "greeter_config.h"
#include "locale_env.h"
#include "proxy_env.h"
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

  // proxy
  using namespace fld;
  CHECK(valid_proxy(""));
  CHECK(valid_proxy("proxy.example.com:3128"));
  CHECK(valid_proxy("http://user:pw@proxy:8080/"));
  CHECK(valid_proxy("socks5h://[::1]:1080"));
  CHECK(valid_proxy("proxy"));
  CHECK(!valid_proxy("proxy:"));
  CHECK(!valid_proxy("proxy:99999"));
  CHECK(!valid_proxy("ftp://proxy:21"));
  CHECK(!valid_proxy("prox y:80"));
  CHECK(!valid_proxy("http://proxy:80/path"));
  CHECK_EQ(proxy_url("p:3128"), std::string("http://p:3128"));
  CHECK_EQ(proxy_url("p:1080", "socks5"), std::string("socks5://p:1080"));
  CHECK_EQ(proxy_url("https://p"), std::string("https://p"));
  CHECK_EQ(proxy_variables().size(), 10u);
  std::map<std::string, std::string> penv{{"HTTP_PROXY", "http://a:1"}, {"PATH", "/bin"}, {"no_proxy", "x"}};
  auto inh = inherited_proxy(penv);
  CHECK_EQ(inh.size(), 2u);
  CHECK(default_mode(inh) == ProxySettings::Inherit);
  CHECK(default_mode({{"no_proxy", "x"}}) == ProxySettings::None);
  ProxySettings ps;
  ps.mode = ProxySettings::Manual;
  ps.http = "p:3128";
  ps.socks = "s:1080";
  ps.no_proxy = "localhost, .lan";
  auto pe = proxy_environment(ps);
  CHECK_EQ(pe.size(), 10u);
  CHECK_EQ(pe[0].second, std::string("http://p:3128"));   // http_proxy
  CHECK_EQ(pe[1].second, std::string(""));                // https_proxy not set
  CHECK_EQ(pe[3].second, std::string("socks5://s:1080")); // all_proxy
  CHECK_EQ(pe[4].second, std::string("localhost,.lan"));  // no_proxy
  CHECK_EQ(pe[5].first, std::string("HTTP_PROXY"));
  CHECK_EQ(pe[5].second, std::string("http://p:3128"));
  ps.use_http_for_all = true;
  pe = proxy_environment(ps);
  CHECK_EQ(pe[1].second, std::string("http://p:3128"));
  CHECK_EQ(pe[3].second, std::string(""));
  ps.mode = ProxySettings::Inherit;
  CHECK_EQ(proxy_environment(ps)[7].second, std::string("@inherit"));
  ps.mode = ProxySettings::None;
  CHECK_EQ(proxy_environment(ps)[0].second, std::string(""));
  Ini pi;
  ps.mode = ProxySettings::Manual;
  save_proxy(pi, ps);
  ProxySettings back = load_proxy(pi);
  CHECK(back.mode == ProxySettings::Manual);
  CHECK_EQ(back.http, ps.http);
  CHECK(back.use_http_for_all);

  // greeter.ini validation
  Ini in, out;
  in.parse("[Greeter]\nMinUid=1000\nHideUsers=guest, test\nKeyboard=always\nBackground=/usr/share/fldesktop/backgrounds/x.svg\n"
           "[Appearance]\nTheme=dark\nAccent=#3584e4\n[Cursor]\nSize=32\n[Other]\nEvil=1\n");
  std::string err;
  CHECK(validate_greeter_config(in, out, err));
  CHECK_EQ(out.get("Greeter", "HideUsers"), std::string("guest, test"));
  CHECK_EQ(out.get("Cursor", "Size"), std::string("32"));
  CHECK(!out.has("Other", "Evil"));
  auto bad = [](const char *text) {
    Ini i, o;
    i.parse(text);
    std::string e;
    return !validate_greeter_config(i, o, e) && !e.empty();
  };
  CHECK(bad("[Greeter]\nMinUid=abc\n"));
  CHECK(bad("[Greeter]\nKeyboard=sometimes\n"));
  CHECK(bad("[Greeter]\nBackground=/home/u/secret.png\n"));
  CHECK(bad("[Greeter]\nBackground=/usr/share/../../etc/shadow\n"));
  CHECK(bad("[Appearance]\nAccent=red\n"));
  CHECK(bad("[General]\nIconTheme=../x\n"));
  CHECK(bad("[Greeter]\nHideUsers=a;rm\n"));
  CHECK(bad("[Cursor]\nSize=500\n"));
  CHECK_EQ(image_type(std::string("\x89PNG\r\n\x1a\nxxxx", 12)), std::string("png"));
  CHECK_EQ(image_type("\xff\xd8\xff\xe0"), std::string("jpg"));
  CHECK_EQ(image_type("<?xml version=\"1.0\"?><svg xmlns=\"http://www.w3.org/2000/svg\"/>"), std::string("svg"));
  CHECK_EQ(image_type("<?xml?><!ENTITY x SYSTEM \"file:///etc/shadow\"><svg/>"), std::string(""));
  CHECK_EQ(image_type("root:x:0:0"), std::string(""));
  // input methods in the session environment
  {
    locale_env::EnvFile ime;
    ime.parse("LANG=ja_JP.UTF-8\nGTK_IM_MODULE=xim\n");
    CHECK_EQ(locale_env::input_method_from_env(ime), std::string(""));
    locale_env::set_input_method(ime, "fcitx5");
    CHECK_EQ(locale_env::input_method_from_env(ime), std::string("fcitx5"));
    CHECK_EQ(ime.get("QT_IM_MODULE"), std::string("fcitx"));
    CHECK_EQ(ime.get("GTK_IM_MODULE"), std::string("")); // text-input-v3 instead
    CHECK_EQ(ime.get("LANG"), std::string("ja_JP.UTF-8"));
    locale_env::set_input_method(ime, "ibus");
    CHECK_EQ(locale_env::input_method_from_env(ime), std::string("ibus"));
    CHECK_EQ(ime.get("GTK_IM_MODULE"), std::string("ibus"));
    locale_env::set_input_method(ime, "");
    CHECK_EQ(locale_env::input_method_from_env(ime), std::string(""));
    CHECK_EQ(ime.get("XMODIFIERS"), std::string(""));
    CHECK_EQ(locale_env::input_method_command("fcitx5"), std::string("fcitx5 -d --replace"));
    CHECK_EQ(locale_env::input_method_command(""), std::string(""));
  }
  return test_result("settings");
}
