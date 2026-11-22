#include <cstdlib>
#include <unistd.h>

#include "fld/accents.h"
#include "fld/desktop_entry.h"
#include "fld/icons.h"
#include "fld/ini.h"
#include "fld/json.h"
#include "fld/mime.h"
#include "fld/styles.h"
#include "fld/tab_bar.h"
#include "fld/menus.h"
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
}

int main() {
  char tmpl[] = "/tmp/fld-test-XXXXXX";
  g_tmp = mkdtemp(tmpl);
  test_util();
  test_ini();
  test_desktop_entry();
  test_json();
  test_mime();
  test_styles();
  test_trash_and_copy();
  test_accents();
  test_session_env();
  test_tabs();
  test_symbolic_recolor();
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
  return test_result("libfld");
}
