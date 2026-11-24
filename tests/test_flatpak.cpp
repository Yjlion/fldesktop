// Flatpak permissions model (and, with libflatpak, the client helpers)
#include <algorithm>

#include "permissions.h"
#include "remotes.h"
#include "test.h"
#ifdef FLD_TEST_FPCLIENT
#include "fpclient.h"
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
  CHECK_EQ(fp::metadata_runtime(meta), std::string("runtime/org.freedesktop.Platform/x86_64/24.08"));
  CHECK_EQ(fp::metadata_runtime("[Runtime]\nname=org.freedesktop.Platform\n"), std::string(""));
  CHECK_EQ(fp::metadata_extension_of("[Runtime]\nname=org.freedesktop.Platform.GL.default\n\n[ExtensionOf]\n"
                                     "ref=runtime/org.freedesktop.Platform/x86_64/24.08\n"),
           std::string("runtime/org.freedesktop.Platform/x86_64/24.08"));
#endif
  return test_result("flatpak");
}
