// fl-panel: mounts removable drives when they are plugged in and offers to
// open them ([Drives] Automount / AutoOpen in desktop.ini).
#include <string>

#include "fld/ini.h"
#include "fld/notify.h"
#include "fld/util.h"
#include "panel.h"
#include "udclient.h"

namespace {

void announce(const ud::Volume &v, const std::string &mount_point) {
  fld::Notification n;
  n.app_name = "Files";
  n.category = "device.added";
  n.icon = v.icon.empty() ? "drive-removable-media" : v.icon;
  n.summary = v.label;
  n.body = mount_point.empty() ? "Connected" : "Ready to use";
  n.transient = true;
  n.actions = {{"default", "Open"}, {"open", "Open"}};
  std::string target = mount_point;
  fld::notify(n, [target](const std::string &) { fld::spawn({"fl-files", target.empty() ? "" : target}); });
}

void added(const ud::Volume &v) {
  if (!v.removable || v.mounted()) return;
  fld::Ini c;
  c.load(fld::desktop_config_path());
  bool automount = c.get_bool("Drives", "Automount", true);
  bool autoopen = c.get_bool("Drives", "AutoOpen", false);
  // encrypted volumes need a passphrase: fl-files asks for it
  if (!automount || !v.hint_auto || v.encrypted) {
    announce(v, "");
    return;
  }
  ud::Volume vol = v;
  ud::Client::get().mount(v.path, [vol, autoopen](const std::string &err, const std::string &mp) {
    if (!err.empty()) {
      fld::Notification n;
      n.app_name = "Files";
      n.summary = "Could not mount " + vol.label;
      n.body = err;
      n.icon = "drive-removable-media";
      fld::notify(n);
      return;
    }
    if (autoopen) fld::spawn({"fl-files", mp});
    else announce(vol, mp);
  });
}

} // namespace

void drives_start() {
  ud::Client::get().on_added = added;
  ud::Client::get().start();
}
