#include <FL/Fl_Image.H>
#include <algorithm>
#include <cstdlib>
#include <memory>
#include <vector>
#include <unistd.h>

#include "fld/accents.h"
#include "fld/avatar.h"
#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/json.h"
#include "fld/mime.h"
#include "fld/styles.h"
#include "fld/theme.h"
#include "fld/config_watch.h"
#include "fld/toolkit_theme.h"
#include "fld/tab_bar.h"
#include "fld/menus.h"
#include "fld/session.h"
#include "fld/drag.h"
#include "fld/fileops.h"
#include "fld/tab_move.h"
#include <FL/Fl_RGB_Image.H>
#include <sys/stat.h>
#include "fld/text.h"
#include "fld/trash.h"
#include "fld/util.h"
#include "test.h"

static std::string g_tmp;

static void test_util() {
  CHECK_EQ(fld::basename("/a/b/c.txt"), std::string("c.txt"));
  CHECK_EQ(fld::basename("/a/b/"), std::string("b"));
  CHECK_EQ(fld::dirname("/a/b/c.txt"), std::string("/a/b"));
  CHECK_EQ(fld::dirname("/a"), std::string("/"));
  CHECK_EQ(fld::extension("x/Photo.JPG"), std::string("jpg"));
  CHECK_EQ(fld::extension(".bashrc"), std::string(""));
  CHECK_EQ(fld::abs_path("/a/./b/../c"), std::string("/a/c"));
  CHECK_EQ(fld::human_size(512), std::string("512 B"));
  CHECK_EQ(fld::human_size(1536), std::string("1.5 KiB"));
  auto w = fld::shell_split("a 'b c' \"d \\\"e\\\"\" f\\ g");
  CHECK_EQ(w.size(), 4u);
  CHECK_EQ(w[1], std::string("b c"));
  CHECK_EQ(w[2], std::string("d \"e\""));
  CHECK_EQ(w[3], std::string("f g"));
  CHECK_EQ(fld::uri_decode(fld::uri_encode_path("/a b/ü%")), std::string("/a b/ü%"));
  CHECK_EQ(fld::uri_encode_path("/a b"), std::string("/a%20b"));
  CHECK_EQ(fld::shell_quote("it's"), std::string("'it'\\''s'"));
  // natural order for file names
  CHECK(fld::natural_compare("file2", "file10") < 0);
  CHECK(fld::natural_compare("file10", "file100") < 0);
  CHECK(fld::natural_compare("file101", "file1000") < 0);
  CHECK(fld::natural_compare("File2", "file10") < 0);
  CHECK(fld::natural_compare("a1", "a01") < 0);
  CHECK(fld::natural_compare("a01", "a2") < 0);
  CHECK(fld::natural_compare("img", "img1") < 0);
  CHECK(fld::natural_compare("x9.png", "x9.jpg") > 0);
  CHECK(fld::natural_compare("99999999999999999999b", "100000000000000000000a") < 0);
  CHECK_EQ(fld::natural_compare("same", "same"), 0);
  CHECK(fld::natural_compare("Abc", "abc") != 0);
}

static void test_ini() {
  fld::Ini ini;
  ini.parse("# c\n[A]\nx = 1\ny=hello world\n[B]\nflag=true\n");
  CHECK_EQ(ini.get("A", "y"), std::string("hello world"));
  CHECK_EQ(ini.get_int("A", "x"), 1);
  CHECK(ini.get_bool("B", "flag"));
  CHECK_EQ(ini.get("B", "missing", "d"), std::string("d"));
  ini.set("C", "k", "v");
  fld::Ini again;
  again.parse(ini.to_string());
  CHECK_EQ(again.get("C", "k"), std::string("v"));
  CHECK_EQ(again.keys("A").size(), 2u);

  // comments and blank lines survive a load / modify / save cycle
  fld::Ini c;
  c.parse("# top comment\n\n[keys]\n# bindings\nSuper+Return=exec fl-term\n\n[pointer]\n; tap\ntap_to_click=true\n");
  c.set("keys", "Super+e", "exec fl-files");
  c.set("pointer", "accel", "0.5");
  std::string out = c.to_string();
  CHECK(out.find("# top comment") == 0);
  CHECK(out.find("# bindings\nSuper+Return=exec fl-term\nSuper+e=exec fl-files\n") != std::string::npos);
  CHECK(out.find("; tap\ntap_to_click=true\naccel=0.5") != std::string::npos);
  CHECK_EQ(c.keys("keys").size(), 2u);
  CHECK_EQ(c.groups().size(), 2u);
  CHECK_EQ(c.get("keys", ""), std::string(""));
  c.remove_group("keys");
  CHECK(c.to_string().find("[keys]") == std::string::npos);
}

static void test_desktop_entry() {
  std::string p = g_tmp + "/app.desktop";
  fld::write_file(p,
                  "[Desktop Entry]\nType=Application\nName=My App\nName[de]=Meine App\n"
                  "Exec=myapp --open %F --name %c\nIcon=myicon\nCategories=Graphics;Viewer;\n"
                  "MimeType=image/png;\nKeywords=photo;picture;\n");
  setenv("LANG", "de_DE.UTF-8", 1);
  fld::DesktopEntry e;
  CHECK(e.load(p));
  CHECK_EQ(e.name, std::string("Meine App"));
  setenv("LANG", "C", 1);
  CHECK(e.load(p));
  CHECK_EQ(e.name, std::string("My App"));
  CHECK_EQ(e.main_category(), std::string("Graphics"));
  CHECK_EQ(e.mime_types.size(), 1u);
  auto cmd = e.command({"/x/a b.png", "/y.png"});
  CHECK_EQ(cmd.size(), 6u);
  CHECK_EQ(cmd[2], std::string("/x/a b.png"));
  CHECK_EQ(cmd[3], std::string("/y.png"));
  CHECK_EQ(cmd[5], std::string("My App"));
  CHECK_EQ(fld::desktop_unescape("a\\sb\\\\c"), std::string("a b\\c"));
  CHECK(e.actions.empty());
  // [Desktop Action] groups, in the order of Actions= (unknown ones skipped)
  std::string p2 = g_tmp + "/act.desktop";
  fld::write_file(p2,
                  "[Desktop Entry]\nType=Application\nName=Web\nExec=web %u\nActions=private;missing;new-window;\n"
                  "[Desktop Action new-window]\nName=New Window\nExec=web --new-window\n"
                  "[Desktop Action private]\nName=New Private Window\nExec=web --private\nIcon=priv\n");
  fld::DesktopEntry a;
  CHECK(a.load(p2));
  CHECK_EQ(a.actions.size(), 2u);
  CHECK_EQ(a.actions[0].id, std::string("private"));
  CHECK_EQ(a.actions[0].name, std::string("New Private Window"));
  CHECK_EQ(a.actions[0].icon, std::string("priv"));
  CHECK_EQ(a.actions[1].exec, std::string("web --new-window"));
  CHECK(!a.launch_action("missing"));
}

static void test_json() {
  fld::Json j;
  CHECK(fld::Json::parse("{\"type\":\"auth_message\",\"n\":3,\"a\":[1,true,null],\"s\":\"\\u00fc\\n\"}", j));
  CHECK_EQ(j["type"].as_string(), std::string("auth_message"));
  CHECK_EQ(j["n"].as_number(), 3.0);
  CHECK_EQ(j["a"].items().size(), 3u);
  CHECK(j["a"].items()[1].as_bool());
  CHECK(j["a"].items()[2].is_null());
  CHECK_EQ(j["s"].as_string(), std::string("ü\n"));
  CHECK(j["missing"].is_null());
  fld::Json k;
  CHECK(fld::Json::parse(j.dump(), k));
  CHECK_EQ(k["s"].as_string(), std::string("ü\n"));
  CHECK(!fld::Json::parse("{\"a\":}", k));
  CHECK(!fld::Json::parse("[1,2", k));
  fld::Json o = fld::Json::object();
  o.set("type", "create_session").set("username", "bob");
  CHECK_EQ(o.dump(), std::string("{\"type\":\"create_session\",\"username\":\"bob\"}"));
}

static void test_mime() {
  if (!fld::file_exists("/usr/share/mime/globs2")) {
    printf("  (shared-mime-info not installed, skipping mime tests)\n");
    return;
  }
  CHECK_EQ(fld::mime_from_name("a.png"), std::string("image/png"));
  CHECK_EQ(fld::mime_from_name("A.PNG"), std::string("image/png"));
  CHECK_EQ(fld::mime_from_name("x.tar.gz"), std::string("application/x-compressed-tar"));
  CHECK_EQ(fld::mime_type(g_tmp), std::string("inode/directory"));
  std::string t = g_tmp + "/noext";
  fld::write_file(t, "hello\n");
  CHECK_EQ(fld::mime_type(t), std::string("text/plain"));
  auto parents = fld::mime_parents("text/x-csrc");
  CHECK(!parents.empty());
}

static void test_trash_and_copy() {
  setenv("XDG_DATA_HOME", (g_tmp + "/data").c_str(), 1);
  std::string dir = g_tmp + "/work";
  fld::mkdir_p(dir + "/sub");
  fld::write_file(dir + "/sub/f.txt", "content");
  std::string err;
  CHECK(fld::copy_recursive(dir, g_tmp + "/copy", &err));
  std::string got;
  CHECK(fld::read_file(g_tmp + "/copy/sub/f.txt", got));
  CHECK_EQ(got, std::string("content"));
  CHECK(!fld::copy_recursive(dir, dir + "/sub/inside", &err)); // into itself
  CHECK(fld::trash(dir + "/sub/f.txt", &err));
  CHECK(!fld::file_exists(dir + "/sub/f.txt"));
  auto items = fld::trash_list();
  CHECK_EQ(items.size(), 1u);
  if (!items.empty()) {
    CHECK_EQ(items[0].original_path, dir + "/sub/f.txt");
    CHECK(fld::trash_restore(items[0], &err));
    CHECK(fld::file_exists(dir + "/sub/f.txt"));
  }
  CHECK(fld::trash_list().empty());
  CHECK_EQ(fld::unique_name(dir + "/sub", "f.txt"), dir + "/sub/f (2).txt");
  CHECK(fld::remove_recursive(g_tmp + "/copy"));
  CHECK(!fld::file_exists(g_tmp + "/copy"));
}

static void test_styles() {
  CHECK(fld::widget_styles().size() >= 8);
  for (auto &st : fld::widget_styles()) CHECK_EQ(fld::normalize_style(st.id), std::string(st.id));
  CHECK_EQ(fld::normalize_style("GTK+"), std::string("gtk+"));
  CHECK_EQ(fld::normalize_style("base"), std::string("none"));
  CHECK_EQ(fld::normalize_style(""), std::string("none"));
  CHECK_EQ(fld::normalize_style(" Fld-Flat "), std::string("fld-flat"));
  CHECK_EQ(fld::normalize_style("bogus"), std::string("gtk+"));
}

static void test_accents() {
  using namespace fld;
  CHECK_EQ(accent_presets().size(), 8u);
  CHECK_EQ(accent_index("#3584E4"), 0);
  CHECK_EQ(accent_index("#123456"), -1);
  CHECK_EQ(nearest_accent("#2e9a4a"), 2);            // a green
  CHECK_EQ(nearest_accent("#ff0000"), 4);            // red
  CHECK_EQ(nearest_accent("#ff8000"), 3);            // orange
  CHECK_EQ(nearest_accent("#808080"), 7);            // gray -> graphite
  CHECK_EQ(nearest_accent("bad"), 0);
  CHECK_EQ(accent_icon_theme(0), std::string("fldesktop"));
  CHECK_EQ(accent_icon_theme(5), std::string("fldesktop-purple"));
  CHECK(is_accent_icon_theme("fldesktop-teal"));
  CHECK(!is_accent_icon_theme("fldesktop-mono"));

  // the icon theme in effect follows the accent, even with a stale IconTheme
  auto eff = [](const char *text) {
    Ini i;
    i.parse(text);
    return effective_icon_theme(i);
  };
  CHECK_EQ(eff("[Appearance]\nAccent=#e62d42\nIconsFollowAccent=true\n[General]\nIconTheme=fldesktop-teal\n"),
           std::string("fldesktop-red"));
  // follow defaults to on for an accent theme
  CHECK_EQ(eff("[Appearance]\nAccent=#e62d42\n[General]\nIconTheme=fldesktop-teal\n"), std::string("fldesktop-red"));
  CHECK_EQ(eff("[Appearance]\nAccent=#e62d42\nIconsFollowAccent=false\n[General]\nIconTheme=fldesktop-teal\n"),
           std::string("fldesktop-teal"));
  CHECK_EQ(eff("[Appearance]\nAccent=#e62d42\n[General]\nIconTheme=fldesktop-mono\n"), std::string("fldesktop-mono"));
  CHECK_EQ(eff("[Appearance]\nAccent=#3584e4\n[General]\nIconTheme=fldesktop-red\n"), std::string("fldesktop"));
  CHECK_EQ(eff("[General]\nIconTheme=fldesktop-teal\n"), std::string("fldesktop-teal")); // no accent set
  Ini miss;
  miss.parse("[Appearance]\nAccent=#e62d42\n[General]\nIconTheme=fldesktop-teal\n");
  CHECK_EQ(effective_icon_theme(miss, [](const std::string &) { return false; }), std::string("fldesktop-teal"));
}

static void test_panel_theme() {
  auto light = [](const char *text) {
    fld::Ini i;
    i.parse(text);
    return fld::panel_is_light_theme(i);
  };
  CHECK(light(""));                                                  // light theme by default
  CHECK(!light("[Appearance]\nTheme=dark\n"));                      // auto follows the theme
  CHECK(!light("[Appearance]\nTheme=light\nPanelTheme=dark\n"));    // dark panel, light windows
  CHECK(light("[Appearance]\nTheme=dark\nPanelTheme=Light\n"));
  CHECK(light("[Appearance]\nTheme=light\nPanelTheme=bogus\n"));
}

static void test_session_env() {
  using namespace fld;
  auto m = parse_env_text("# x\nhttp_proxy=http://p:3128\nexport NO_PROXY='localhost'\nbad\nE=\n");
  CHECK_EQ(m.size(), 3u);
  CHECK_EQ(m["http_proxy"], std::string("http://p:3128"));
  CHECK_EQ(m["NO_PROXY"], std::string("localhost"));
  CHECK_EQ(m["E"], std::string(""));
  // wm.ini [environment] with a snapshot of the session start
  std::string cfg = std::string(g_tmp) + "/cfg", run = std::string(g_tmp) + "/run";
  mkdir_p(cfg + "/fldesktop");
  mkdir_p(run);
  setenv("XDG_CONFIG_HOME", cfg.c_str(), 1);
  setenv("XDG_RUNTIME_DIR", run.c_str(), 1);
  write_file(cfg + "/fldesktop/wm.ini", "[environment]\nhttp_proxy=@inherit\nhttps_proxy=@inherit\nall_proxy=\nFOO=bar\n");
  write_file(run + "/fldesktop-session.env", "http_proxy=http://old:8080\n");
  auto ch = session_env_changes();
  CHECK_EQ(ch.size(), 4u);
  if (ch.size() == 4) {
    CHECK_EQ(ch[0].second, std::string("http://old:8080"));
    CHECK_EQ(ch[1].second, std::string(1, '\x01'));  // not in the snapshot: unset
    CHECK_EQ(ch[2].second, std::string(1, '\x01'));  // empty: unset
    CHECK_EQ(ch[3].second, std::string("bar"));
  }
  unsetenv("XDG_CONFIG_HOME");
  unsetenv("XDG_RUNTIME_DIR");
}

static void test_tabs() {
  std::vector<int> v = {0, 1, 2, 3};
  fld::move_element(v, 0, 2);
  CHECK(v == std::vector<int>({1, 2, 0, 3}));
  fld::move_element(v, 3, 0);
  CHECK(v == std::vector<int>({3, 1, 2, 0}));
  fld::move_element(v, 1, 9); // out of range: unchanged
  CHECK(v == std::vector<int>({3, 1, 2, 0}));
  // the current index follows the tabs
  CHECK_EQ(fld::moved_index(0, 0, 2), 2);
  CHECK_EQ(fld::moved_index(1, 0, 2), 0);
  CHECK_EQ(fld::moved_index(3, 0, 2), 3);
  CHECK_EQ(fld::moved_index(1, 3, 0), 2);
  // drop position: past half of the next tab
  CHECK_EQ(fld::TabBar::drop_index(0, 0, 100, 4), 0);
  CHECK_EQ(fld::TabBar::drop_index(0, 49, 100, 4), 0);
  CHECK_EQ(fld::TabBar::drop_index(0, 51, 100, 4), 1);
  CHECK_EQ(fld::TabBar::drop_index(0, 1000, 100, 4), 3);
  CHECK_EQ(fld::TabBar::drop_index(2, -300, 100, 4), 0);
  CHECK_EQ(fld::Menu::text("a & b @ c"), std::string("a && b @@ c"));
}

static void test_symbolic_recolor() {
  const std::string W = "#eeeeee";
  CHECK_EQ(fld::recolor_symbolic_svg("<path fill=\"#2e3436\"/>", W), "<path fill=\"#eeeeee\"/>");
  CHECK_EQ(fld::recolor_symbolic_svg("<path style=\"fill:#bebebe;stroke:#474747\"/>", W),
           "<path style=\"fill:#eeeeee;stroke:#eeeeee\"/>");
  CHECK_EQ(fld::recolor_symbolic_svg("<g fill=\"#333\" fill-opacity=\"0.4\"/>", W), "<g fill=\"#eeeeee\" fill-opacity=\"0.4\"/>");
  CHECK_EQ(fld::recolor_symbolic_svg("<g color=\"currentColor\"/>", W), "<g color=\"#eeeeee\"/>");
  // colors, white and references stay
  CHECK_EQ(fld::recolor_symbolic_svg("<path fill=\"#e01b24\"/>", W), "<path fill=\"#e01b24\"/>");
  CHECK_EQ(fld::recolor_symbolic_svg("<path fill=\"#ffffff\"/>", W), "<path fill=\"#ffffff\"/>");
  CHECK_EQ(fld::recolor_symbolic_svg("<use href=\"#abc\" fill=\"url(#aaa)\"/>", W).find("url(#aaa)") != std::string::npos, true);
  // single-colour icons that ui_icon() draws in the text colour on dark themes
  CHECK(fld::svg_is_monochrome("<svg><rect fill=\"#3d4046\"/><path stroke=\"#3d4046\"/></svg>"));
  CHECK(fld::svg_is_monochrome("<svg><path d=\"M0 0h4v4z\"/></svg>")); // the default black
  CHECK(fld::svg_is_monochrome("<svg><path fill=\"#222\"/><path fill=\"#fff\"/></svg>"));
  CHECK(!fld::svg_is_monochrome("<svg><path fill=\"#3584e4\"/></svg>"));
  CHECK(!fld::svg_is_monochrome("<svg><path fill=\"#222\"/><path fill=\"#c0c0c0\"/></svg>"));
  CHECK(!fld::svg_is_monochrome("<svg><linearGradient id=\"g\"/><path fill=\"url(#g)\"/></svg>"));
}

// 10 px per character (not byte), so the tests need no fonts
static double mono10(const std::string &s) {
  int n = 0;
  for (unsigned char c : s) n += (c & 0xC0) != 0x80;
  return n * 10.0;
}

static void test_text() {
  CHECK_EQ(fld::ellipsize("short", 100, mono10), std::string("short"));
  CHECK_EQ(fld::ellipsize("alsa-card-profile", 60, mono10), std::string("alsa-\u2026"));
  CHECK_EQ(fld::ellipsize("\u00e9\u00e9\u00e9\u00e9\u00e9\u00e9", 40, mono10), std::string("\u00e9\u00e9\u00e9\u2026"));
  CHECK_EQ(fld::ellipsize("ab cdef", 40, mono10), std::string("ab\u2026")); // no space before "…"
  // icon labels break at separators before characters
  auto l = fld::wrap_label("alsa-card-profile", 100, 2, mono10);
  CHECK_EQ(l.size(), 2u);
  if (l.size() == 2) {
    CHECK_EQ(l[0], std::string("alsa-card-"));
    CHECK_EQ(l[1], std::string("profile"));
  }
  l = fld::wrap_label("bash-completion", 100, 2, mono10);
  CHECK(l.size() == 2 && l[0] == "bash-" && l[1] == "completion");
  l = fld::wrap_label("My Documents", 80, 2, mono10);
  CHECK(l.size() == 2 && l[0] == "My" && l[1] == "Documen\u2026");
  l = fld::wrap_label("abcdefghijkl", 50, 2, mono10); // no separator: between characters
  CHECK(l.size() == 2 && l[0] == "abcde" && l[1] == "fghi\u2026");
  l = fld::wrap_label("one two three four five", 50, 2, mono10);
  CHECK(l.size() == 2 && l[0] == "one" && fld::ends_with(l[1], "\u2026"));
  CHECK_EQ(fld::wrap_label("x", 50, 2, mono10).size(), 1u);
  CHECK(fld::wrap_label("", 50, 2, mono10).empty());
  // text colour on an accent: amber takes dark text, a deep blue white
  CHECK(fld::text_on(fl_rgb_color(0xc8, 0x88, 0x00)) != FL_WHITE);
  CHECK(fld::text_on(fl_rgb_color(0x1c, 0x71, 0xd8)) == FL_WHITE);
  CHECK(fld::text_on(FL_WHITE) != FL_WHITE);
  CHECK_NEAR(fld::contrast_ratio(FL_BLACK, FL_WHITE), 21.0);
}

// icon lookup in a theme listed once: the best size, the parent, a miss
static void test_icon_lookup() {
  std::string data = g_tmp + "/icondata";
  std::string th = data + "/icons/fldtest-theme";
  fld::mkdir_p(th + "/16x16/apps");
  fld::mkdir_p(th + "/48x48/apps");
  fld::mkdir_p(th + "/scalable/apps");
  fld::write_file(th + "/index.theme",
                  "[Icon Theme]\nName=T\nInherits=fldtest-parent\nDirectories=16x16/apps,48x48/apps,scalable/apps\n"
                  "[16x16/apps]\nSize=16\nType=Fixed\n[48x48/apps]\nSize=48\nType=Fixed\n"
                  "[scalable/apps]\nSize=48\nMinSize=8\nMaxSize=512\nType=Scalable\n");
  const std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'/>";
  fld::write_file(th + "/16x16/apps/foo.png", "x");
  fld::write_file(th + "/48x48/apps/foo.png", "x");
  fld::write_file(th + "/scalable/apps/bar.svg", svg);
  fld::write_file(th + "/scalable/apps/masked.svg", "<svg><mask id='m'/></svg>");
  std::string pt = data + "/icons/fldtest-parent";
  fld::mkdir_p(pt + "/48x48/apps");
  fld::write_file(pt + "/index.theme", "[Icon Theme]\nName=P\nDirectories=48x48/apps\n[48x48/apps]\nSize=48\nType=Fixed\n");
  fld::write_file(pt + "/48x48/apps/inherited.png", "x");
  setenv("XDG_DATA_HOME", data.c_str(), 1);
  fld::set_icon_theme_name("fldtest-theme");
  CHECK_EQ(fld::icon_path("foo", 48), th + "/48x48/apps/foo.png");
  CHECK_EQ(fld::icon_path("foo", 16), th + "/16x16/apps/foo.png");
  CHECK_EQ(fld::icon_path("foo.png", 20), th + "/16x16/apps/foo.png"); // the closest
  CHECK_EQ(fld::icon_path("bar", 100), th + "/scalable/apps/bar.svg");
  CHECK_EQ(fld::icon_path("inherited", 48), pt + "/48x48/apps/inherited.png");
  CHECK_EQ(fld::icon_path("masked", 48), std::string("")); // nanosvg cannot draw it
  CHECK_EQ(fld::icon_path("fldtest-no-such-icon", 48), std::string(""));
  fld::set_icon_theme_name("");
  unsetenv("XDG_DATA_HOME");
}

// AccountsService pictures have no extension: loading one must give an
// image the caller owns (it used to hand out FLTK's shared copy, which the
// caller then deleted, and the next load crashed)
static void test_load_image_no_extension() {
  static unsigned char px[4 * 4 * 3];
  for (size_t i = 0; i < sizeof px; i++) px[i] = (unsigned char)(i * 7);
  Fl_RGB_Image src(px, 4, 4, 3);
  std::string png;
  CHECK(fld::avatar_png(&src, 8, png));
  std::string path = g_tmp + "/picture-no-ext";
  fld::write_file(path, png);
  for (int i = 0; i < 3; i++) {
    std::unique_ptr<Fl_Image> img(fld::load_image(path));
    CHECK(img != nullptr);
    if (!img) return;
    CHECK_EQ(img->w(), 8);
    CHECK(dynamic_cast<Fl_RGB_Image *>(img.get()) != nullptr);
  }
  std::string odd = g_tmp + "/picture.dat"; // unknown extension: sniffed
  fld::write_file(odd, png);
  std::unique_ptr<Fl_Image> img(fld::load_image(odd));
  CHECK(img != nullptr);
  fld::write_file(odd, "not an image");
  CHECK(fld::load_image(odd) == nullptr);
}

static void test_toolkit_theme() {
  fld::Ini d;
  d.parse("[Appearance]\nTheme=dark\nAccent=#e01b24\nFont=Cantarell\nFontSize=16\n[General]\nIconTheme=Papirus\n");
  fld::ToolkitTheme t = fld::toolkit_theme_from(d);
  CHECK(t.dark);
  CHECK_EQ(t.accent, std::string("#e01b24"));
  CHECK_EQ(t.accent_fg, std::string("#ffffff"));
  CHECK_EQ(t.font_pt, 12);
  CHECK_EQ(t.icon_theme, std::string("Papirus"));
  // the marked block is replaced; the user's own CSS stays, in place
  std::string block = fld::gtk_accent_css(t, 3);
  CHECK(block.find("@define-color theme_selected_bg_color #e01b24;") != std::string::npos);
  std::string css = fld::replace_marked_block("label { color: red; }\n", block);
  CHECK(css.find("label { color: red; }") != std::string::npos);
  CHECK(css.find(block) == 0);
  fld::Ini d2;
  d2.parse("[Appearance]\nAccent=#33d17a\n");
  std::string again = fld::replace_marked_block(css, fld::gtk_accent_css(fld::toolkit_theme_from(d2), 3));
  CHECK(again.find("#e01b24") == std::string::npos);
  CHECK(again.find("#33d17a") != std::string::npos);
  CHECK(again.find("label { color: red; }") != std::string::npos);
  CHECK_EQ(fld::replace_marked_block(again, fld::gtk_accent_css(fld::toolkit_theme_from(d2), 3)), again);
  // qt5ct: 21 colour roles, qt6ct: 22 (Accent)
  std::string q5 = fld::qtct_color_scheme(t, 5), q6 = fld::qtct_color_scheme(t, 6);
  auto roles = [](const std::string &s) {
    size_t a = s.find("active_colors="), e = s.find('\n', a);
    std::string line = s.substr(a, e - a);
    return (int)std::count(line.begin(), line.end(), ',') + 1;
  };
  CHECK_EQ(roles(q5), 21);
  CHECK_EQ(roles(q6), 22);
  CHECK(q5.find("#ffe01b24") != std::string::npos);
  // writing: settings.ini keeps other keys; a user's own qt5ct setup is left alone
  std::string cfg = g_tmp + "/tkconfig";
  fld::mkdir_p(cfg + "/gtk-3.0");
  fld::write_file(cfg + "/gtk-3.0/settings.ini", "[Settings]\ngtk-cursor-theme-name=Foo\n");
  fld::mkdir_p(cfg + "/qt5ct");
  fld::write_file(cfg + "/qt5ct/qt5ct.conf", "[Appearance]\ncolor_scheme_path=/x/mine.conf\ncustom_palette=true\n");
  CHECK(fld::export_toolkit_theme(d, cfg));
  fld::Ini g3;
  g3.load(cfg + "/gtk-3.0/settings.ini");
  CHECK_EQ(g3.get("Settings", "gtk-cursor-theme-name"), std::string("Foo"));
  CHECK_EQ(g3.get("Settings", "gtk-application-prefer-dark-theme"), std::string("1"));
  fld::Ini q;
  q.load(cfg + "/qt5ct/qt5ct.conf");
  CHECK_EQ(q.get("Appearance", "color_scheme_path"), std::string("/x/mine.conf"));
  q.load(cfg + "/qt6ct/qt6ct.conf");
  CHECK_EQ(q.get("Appearance", "color_scheme_path"), cfg + "/qt6ct/colors/fldesktop.conf");
  std::string g4;
  CHECK(fld::read_file(cfg + "/gtk-4.0/gtk.css", g4) && g4.find("--accent-bg-color: #e01b24") != std::string::npos);
  d.set_bool("Appearance", "ToolkitColors", false);
  CHECK(!fld::export_toolkit_theme(d, cfg));
}

// the first Flatpak creates .../flatpak/exports/share/applications: a
// watch set up before that must still notice it, and what goes in it
static void test_dir_watcher() {
  std::string root = g_tmp + "/dw", target = root + "/flatpak/exports/share/applications";
  fld::mkdir_p(root);
  int calls = 0;
  fld::DirWatcher w({target}, [&] { calls++; }, 0);
  CHECK_EQ(w.watched().size(), (size_t)1);
  fld::mkdir_p(root + "/flatpak/exports");
  w.poll();
  fld::mkdir_p(target);
  w.poll(); // the parents appear one by one: follow them down
  w.poll();
  CHECK(calls >= 1);
  int before = calls;
  fld::write_file(target + "/org.example.App.desktop", "[Desktop Entry]\n");
  w.poll();
  CHECK(calls > before);
  // removed and created again
  unlink((target + "/org.example.App.desktop").c_str());
  rmdir(target.c_str());
  w.poll();
  fld::mkdir_p(target);
  w.poll();
  before = calls;
  fld::write_file(target + "/b.desktop", "x");
  w.poll();
  CHECK(calls > before);
}

int main() {
  char tmpl[] = "/tmp/fld-test-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  test_load_image_no_extension();
  test_toolkit_theme();
  test_dir_watcher();
  test_icon_lookup();
  test_util();
  test_ini();
  test_desktop_entry();
  test_json();
  test_mime();
  test_styles();
  test_trash_and_copy();
  test_accents();
  test_panel_theme();
  test_session_env();
  test_tabs();
  test_symbolic_recolor();
  test_text();
  fld::remove_recursive(g_tmp);
  // clock formats: seconds or not
  CHECK(!fld::strftime_has_seconds("%a %d %b  %H:%M"));
  CHECK(fld::strftime_has_seconds("%H:%M:%S"));
  CHECK(fld::strftime_has_seconds("%T"));
  CHECK(fld::strftime_has_seconds("%-S"));
  CHECK(fld::strftime_has_seconds("%OS"));
  CHECK(fld::strftime_has_seconds("%c"));
  CHECK(!fld::strftime_has_seconds("100%% %H"));
  CHECK(!fld::strftime_has_seconds("%%S"));
  CHECK(!fld::strftime_has_seconds("%"));
  {
    double next = fld::strftime_next_change("%H:%M", 1000000.25);
    CHECK(next > 0 && next <= 60);
    CHECK_NEAR(fld::strftime_next_change("%H:%M:%S", 1000000.25), 0.75);
  }
  // power actions without logind on the bus: systemctl (systemd's loginctl has
  // no power verbs), then elogind's loginctl
  CHECK_EQ(fld::power_command("poweroff", true, true)[0], std::string("systemctl"));
  CHECK_EQ(fld::power_command("reboot", true, false)[0], std::string("loginctl"));
  CHECK_EQ(fld::power_command("reboot", true, false)[1], std::string("reboot"));
  CHECK_EQ(fld::power_command("suspend", false, true)[0], std::string("systemctl"));
  CHECK_EQ(fld::power_command("suspend", false, true)[1], std::string("suspend"));
  CHECK(fld::power_command("reboot", false, false).empty());

  // moving tabs: payloads, one claimer wins, a refused tab goes back
  {
    char tmpl[] = "/tmp/fld-tabs-XXXXXX";
    std::string rt = mkdtemp(tmpl);
    setenv("XDG_RUNTIME_DIR", rt.c_str(), 1);
    std::string file = fld::write_tab_file("fl-files", "{\"cwd\":\"/tmp\"}", "tmp");
    CHECK(!file.empty());
    struct stat st;
    CHECK(stat(file.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    std::string f;
    CHECK(fld::parse_tab_payload(std::string(fld::kTabPayloadPrefix) + file, f));
    CHECK_EQ(f, file);
    CHECK(!fld::parse_tab_payload("file:///etc/passwd", f));
    CHECK(!fld::parse_tab_payload(std::string(fld::kTabPayloadPrefix) + "/etc/passwd", f));
    CHECK(!fld::parse_tab_payload(std::string(fld::kTabPayloadPrefix) + fld::tab_dir() + "/../x", f));
    std::string state;
    int pid = 0;
    CHECK(!fld::claim_tab_file(file, "fl-edit", state)); // another program's tab
    CHECK(fld::claim_tab_file(file, "fl-files", state, &pid));
    CHECK_EQ(state, std::string("{\"cwd\":\"/tmp\"}"));
    CHECK_EQ(pid, (int)getpid());
    CHECK(!fld::claim_tab_file(file, "fl-files", state)); // only once
    CHECK(fld::restore_tab_file(file, "fl-files", state, pid));
    CHECK(fld::claim_tab_file(file, "fl-files", state));
    // the payload is no file list: dropped on the desktop it moves nothing
    CHECK(fld::parse_uri_list(std::string(fld::kTabPayloadPrefix) + file).empty());
    CHECK(system(("rm -rf '" + rt + "'").c_str()) == 0);
  }

  // drag images: a chip for a tab, stacked icons with a count badge
  {
    fld::DragImage chip;
    chip.label = "Documents";
    fld::DragPixels a = fld::render_drag_image(chip, 1, false);
    CHECK(a.w > 60 && a.h >= 28);
    CHECK_EQ(a.argb.size(), (size_t)a.w * a.h);
    CHECK(a.argb[(size_t)(a.h / 2) * a.w + a.w / 2] >> 24 > 0); // the chip is drawn
    CHECK_EQ(a.argb[0] >> 24, 0u); // its corner stays clear
    fld::DragPixels a2 = fld::render_drag_image(chip, 2, false);
    CHECK_EQ(a2.w, a.w * 2);
    static uchar px[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; i++) { px[i * 4] = 200; px[i * 4 + 1] = 40; px[i * 4 + 2] = 40; px[i * 4 + 3] = 255; }
    Fl_RGB_Image icon(px, 16, 16, 4);
    fld::DragImage many;
    many.icons = {&icon, &icon, &icon, &icon};
    many.count = 12;
    many.size = 32;
    fld::DragPixels b = fld::render_drag_image(many, 1, false);
    CHECK(b.w > 32 && b.h > 32);
    CHECK(b.hot_x > 0 && b.hot_y > 0 && b.hot_x < b.w && b.hot_y < b.h);
    uint32_t front = b.argb[(size_t)(b.h - 3) * b.w + 3]; // inside the front icon
    CHECK((front >> 24) > 150 && ((front >> 16) & 0xff) > ((front >> 8) & 0xff));
    fld::DragPixels c = fld::render_drag_image(many, 1, true);
    CHECK(c.argb != b.argb); // the copy badge
  }

  // user pictures: a stable color, the initial, a round copy, square PNGs
  {
    CHECK_EQ(fld::avatar_color_hex("alice"), fld::avatar_color_hex("alice"));
    CHECK(fld::accent_index(fld::avatar_color_hex("bob")) >= 0);
    CHECK_EQ(fld::avatar_initial("alice", ""), std::string("A"));
    CHECK_EQ(fld::avatar_initial("alice", "  émile Zola"), std::string("\xc3\x89")); // É
    CHECK_EQ(fld::avatar_initial("", ""), std::string("?"));
    // a 40x20 red picture: covered, cropped to 16x16, round
    std::vector<uchar> px(40 * 20 * 3);
    for (size_t i = 0; i < px.size(); i += 3) px[i] = 255;
    Fl_RGB_Image src(px.data(), 40, 20, 3);
    std::unique_ptr<Fl_RGB_Image> r(fld::avatar_image(&src, 16));
    CHECK(r && r->w() == 16 && r->h() == 16 && r->d() == 4);
    if (r) {
      const uchar *d = (const uchar *)r->data()[0];
      CHECK_EQ((int)d[3], 0);                           // corner: outside the circle
      CHECK_EQ((int)d[(8 * 16 + 8) * 4 + 3], 255);      // middle: opaque
      CHECK_EQ((int)d[(8 * 16 + 8) * 4 + 0], 255);      // and red
    }
    std::string png;
    CHECK(fld::avatar_png(&src, 32, png));
    CHECK(png.size() > 8 && png.compare(1, 3, "PNG") == 0);
    CHECK(png.size() > 24 && (uchar)png[19] == 32 && (uchar)png[23] == 32); // IHDR 32x32
    CHECK(fld::initial_avatar_png("alice", "Alice", 64, png));
    CHECK(png.size() > 24 && png.compare(1, 3, "PNG") == 0 && (uchar)png[19] == 64);
  }
  {
    // background helpers end with the session
    std::string saved = getenv("WAYLAND_DISPLAY") ? getenv("WAYLAND_DISPLAY") : "";
    bool had = getenv("WAYLAND_DISPLAY") != nullptr;
    unsetenv("WAYLAND_DISPLAY");
    CHECK(fld::wayland_display_alive()); // nothing to check
    setenv("WAYLAND_DISPLAY", "/nonexistent/fld-test-wayland-0", 1);
    CHECK(!fld::wayland_display_alive());
    if (had) setenv("WAYLAND_DISPLAY", saved.c_str(), 1);
    else unsetenv("WAYLAND_DISPLAY");
    CHECK(fld::total_ram_bytes() > (64ull << 20));
  }
  return test_result("libfld");
}
