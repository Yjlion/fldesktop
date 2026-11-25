// Flatpak permissions model (and, with libflatpak, the client helpers)
#include <algorithm>

#include "permissions.h"
#include "remotes.h"
#include "test.h"
#ifdef FLD_TEST_FPCLIENT
#include "fpclient.h"
#include "fld/util.h"
#include <unistd.h>
#endif

static void test_remotes() {
  using V = std::vector<std::string>;
  CHECK(fp::remote_add_cmd("user", "beta", "https://dl.flathub.org/beta-repo/flathub-beta.flatpakrepo", "Flathub Beta", "") ==
        V({"flatpak", "remote-add", "--user", "--if-not-exists", "--title=Flathub Beta", "--from", "beta",
           "https://dl.flathub.org/beta-repo/flathub-beta.flatpakrepo"}));
  CHECK(fp::remote_add_cmd("system", "mine", "https://example.org/repo", "", "-") ==
        V({"flatpak", "remote-add", "--system", "--if-not-exists", "--no-gpg-verify", "mine", "https://example.org/repo"}));
  CHECK(fp::remote_add_cmd("system", "k", "https://example.org/repo", "", "/tmp/key.gpg") ==
        V({"flatpak", "remote-add", "--system", "--if-not-exists", "--gpg-import=/tmp/key.gpg", "k",
           "https://example.org/repo"}));
  fp::Remote r;
  r.name = "flathub";
  r.installation = "system";
  r.prio = 5;
  r.disabled = true;
  r.title = "Flathub";
  CHECK(fp::remote_modify_cmd(r) ==
        V({"flatpak", "remote-modify", "--system", "--disable", "--prio=5", "--title=Flathub", "flathub"}));
  CHECK(fp::remote_delete_cmd(r, true) == V({"flatpak", "remote-delete", "--system", "--force", "flathub"}));
  CHECK_EQ(fp::remote_name_for("Flathub Beta"), std::string("flathub-beta"));
  CHECK_EQ(fp::remote_name_for("https://dl.flathub.org/beta-repo/flathub-beta.flatpakrepo"), std::string("flathub-beta"));
  CHECK_EQ(fp::remote_name_for("https://dl.example.org/repo/"), std::string("example"));
  CHECK_EQ(fp::remote_name_for("/home/me/Downloads/gnome-nightly.flatpakrepo"), std::string("gnome-nightly"));
  CHECK_EQ(fp::remote_name_for("!!!"), std::string("source"));
}

int main() {
  test_remotes();
  const char *meta = "[Application]\nname=org.example.App\nruntime=org.freedesktop.Platform/x86_64/24.08\n\n"
                     "[Context]\nshared=network;ipc;\nsockets=x11;wayland;pulseaudio;\ndevices=dri;\n"
                     "filesystems=xdg-download;home:ro;\n\n[Environment]\nFOO=bar\n";
  fp::Permissions p;
  fp::parse_metadata(meta, p);
  CHECK(p.effective("shared", "network"));
  CHECK(p.effective("sockets", "x11"));
  CHECK(!p.effective("sockets", "cups"));
  CHECK(p.effective("devices", "dri"));
  CHECK_EQ(p.app_filesystems.size(), (size_t)2);
  CHECK_EQ(p.app_env["FOO"], std::string("bar"));
  // existing overrides from `flatpak override --user`
  fp::parse_overrides("[Context]\nshared=!network;\nfilesystems=!home;~/Games;\n", p);
  CHECK(!p.effective("shared", "network"));
  CHECK(p.overridden("shared", "network"));
  auto fs = p.filesystems();
  CHECK_EQ(fs.size(), (size_t)2); // xdg-download, ~/Games
  CHECK(std::find(fs.begin(), fs.end(), "home:ro") == fs.end());
  // setting back to the app's value removes the override
  p.set("shared", "network", true);
  CHECK(!p.overridden("shared", "network"));
  p.set("sockets", "x11", false);
  p.set("sockets", "cups", true);
  p.add_filesystem("xdg-music:ro");
  p.remove_filesystem("xdg-download");
  std::string out = fp::write_overrides("# kept\n[Context]\nshared=!network;\n\n[Session Bus Policy]\norg.foo=talk\n", p);
  CHECK(out.find("sockets=cups;!x11;") != std::string::npos);
  CHECK(out.find("shared=") == std::string::npos);
  CHECK(out.find("org.foo=talk") != std::string::npos);
  CHECK(out.find("# kept") != std::string::npos);
  CHECK(out.find("!xdg-download") != std::string::npos);
  CHECK(out.find("xdg-music:ro") != std::string::npos);
  CHECK(out.find("!home") != std::string::npos);
  fp::Permissions q;
  fp::parse_metadata(meta, q);
  fp::parse_overrides(out, q);
  CHECK(!q.effective("sockets", "x11"));
  CHECK(q.effective("sockets", "cups"));
  q.reset();
  CHECK(fp::write_overrides("", q).find("[Context]") == std::string::npos);
  CHECK_EQ(fp::filesystem_label("xdg-download:ro"), std::string("Downloads folder (read only)"));
  CHECK_EQ(fp::filesystem_label("xdg-documents/Work"), std::string("Documents folder Work"));
  CHECK_EQ(fp::filesystem_label("/opt/data"), std::string("/opt/data"));
#ifdef FLD_TEST_FPCLIENT
  CHECK_EQ(fp::ref_id("app/org.gnome.Calculator/x86_64/stable"), std::string("org.gnome.Calculator"));
  CHECK_EQ(fp::strip_markup("<p>A calculator.</p><ul><li>Fast</li><li>Small &amp; nice</li></ul>"),
           std::string("A calculator.\n\n  • Fast\n  • Small & nice"));
  CHECK_EQ(fp::category_label("Game"), std::string("Games"));
  {
    // the catalog parser: languages, bundles, descriptions, icons
    std::string dir = "/tmp/fld-test-appstream-" + std::to_string(getpid());
    fld::mkdir_p(dir + "/icons/128x128");
    fld::mkdir_p(dir + "/icons/64x64");
    fld::write_file(dir + "/icons/128x128/org.example.Paint.png", "x");
    fld::write_file(dir + "/icons/64x64/org.example.Paint.png", "x");
    std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<components version="1.0" origin="flathub">
  <component type="desktop-application">
    <id>org.example.Paint</id>
    <name>Paint</name>
    <name xml:lang="de">Malen</name>
    <summary>Draw &amp; paint</summary>
    <summary xml:lang="de-AT">Zeichnen</summary>
    <summary xml:lang="de_CH">Zeichnen (CH)</summary>
    <developer id="example.org"><name>Example Team</name></developer>
    <project_license>GPL-3.0-or-later</project_license>
    <url type="bugtracker">https://example.org/bugs</url>
    <url type="homepage">https://example.org</url>
    <description>
      <p>A <em>simple</em> paint program.</p>
      <p xml:lang="de">Ein Malprogramm.</p>
      <ul><li>Layers</li><li xml:lang="de">Ebenen</li><li>Brushes</li></ul>
      <heading>More</heading>
    </description>
    <categories><category>Graphics</category><category>2DGraphics</category></categories>
    <icon type="stock">org.example.Paint</icon>
    <icon type="cached" width="64" height="64">org.example.Paint.png</icon>
    <icon type="cached" width="128" height="128">org.example.Paint.png</icon>
    <icon type="remote" width="64" height="64">https://example.org/64.png</icon>
    <bundle type="flatpak" runtime="org.freedesktop.Platform/x86_64/24.08">app/org.example.Paint/x86_64/stable</bundle>
    <releases>
      <release version="1.2" timestamp="1710000000"><description><p>Release notes</p></description></release>
      <release version="1.1" timestamp="1700000000"/>
    </releases>
  </component>
  <component type="desktop">
    <id>org.example.Old</id>
    <developer_name>Old Dev</developer_name>
    <bundle type="flatpak">app/org.example.Old/x86_64/stable</bundle>
  </component>
  <component type="runtime"><id>org.example.Platform</id><bundle type="flatpak">runtime/org.example.Platform/x86_64/1</bundle></component>
  <component type="desktop-application"><id>org.example.Deb</id><name>No bundle</name></component>
  <component type="addon"><id>org.example.Paint.Plugin</id><bundle type="flatpak">runtime/org.example.Paint.Plugin/x86_64/stable</bundle></component>
</components>
)";
    std::map<std::string, fp::App> apps;
    CHECK(fp::parse_appstream_text("flathub", dir, xml, {""}, apps));
    CHECK_EQ(apps.size(), (size_t)2);
    const fp::App &a = apps["org.example.Paint"];
    CHECK_EQ(a.name, std::string("Paint"));
    CHECK_EQ(a.summary, std::string("Draw & paint"));
    CHECK_EQ(a.description, std::string("A simple paint program.\n\n  • Layers\n  • Brushes\n\nMore"));
    CHECK_EQ(a.developer, std::string("Example Team"));
    CHECK_EQ(a.license, std::string("GPL-3.0-or-later"));
    CHECK_EQ(a.homepage, std::string("https://example.org"));
    CHECK_EQ(a.version, std::string("1.2"));
    CHECK_EQ(a.ref, std::string("app/org.example.Paint/x86_64/stable"));
    CHECK_EQ(a.remote, std::string("flathub"));
    CHECK(a.in_catalog);
    CHECK_EQ(a.categories.size(), (size_t)2);
    CHECK_EQ(a.icon, dir + "/icons/128x128/org.example.Paint.png");
    CHECK_EQ(a.icon_stock, std::string("org.example.Paint"));
    CHECK_EQ(a.icon_url, std::string("https://example.org/64.png"));
    CHECK_EQ(apps["org.example.Old"].name, std::string("org.example.Old")); // no name: the id
    CHECK_EQ(apps["org.example.Old"].developer, std::string("Old Dev"));
    // German (Austria): the closest translation of each text, else the original
    std::map<std::string, fp::App> de;
    CHECK(fp::parse_appstream_text("flathub", dir, xml, {"de_AT", "de", ""}, de));
    CHECK_EQ(de["org.example.Paint"].name, std::string("Malen"));
    CHECK_EQ(de["org.example.Paint"].summary, std::string("Zeichnen"));
    CHECK_EQ(de["org.example.Paint"].description, std::string("Ein Malprogramm.\n\n  • Ebenen"));
    // German alone: another country's German beats the original
    std::map<std::string, fp::App> de2;
    CHECK(fp::parse_appstream_text("flathub", dir, xml, {"de", ""}, de2));
    CHECK(de2["org.example.Paint"].summary.rfind("Zeichnen", 0) == 0);
    CHECK_EQ(de2["org.example.Paint"].name, std::string("Malen"));
    // a catalog cut short keeps the components before the cut
    std::map<std::string, fp::App> cut;
    CHECK(!fp::parse_appstream_text("flathub", dir, xml.substr(0, xml.find("<component type=\"runtime\">") + 30), {""}, cut));
    CHECK_EQ(cut.size(), (size_t)2);
    for (const char *f : {"/icons/128x128/org.example.Paint.png", "/icons/64x64/org.example.Paint.png"})
      unlink((dir + f).c_str());
    for (const char *d : {"/icons/128x128", "/icons/64x64", "/icons", ""}) rmdir((dir + d).c_str());
  }
  CHECK_EQ(fp::metadata_runtime(meta), std::string("runtime/org.freedesktop.Platform/x86_64/24.08"));
  CHECK_EQ(fp::metadata_runtime("[Runtime]\nname=org.freedesktop.Platform\n"), std::string(""));
  CHECK_EQ(fp::metadata_extension_of("[Runtime]\nname=org.freedesktop.Platform.GL.default\n\n[ExtensionOf]\n"
                                     "ref=runtime/org.freedesktop.Platform/x86_64/24.08\n"),
           std::string("runtime/org.freedesktop.Platform/x86_64/24.08"));
#endif
  return test_result("flatpak");
}
