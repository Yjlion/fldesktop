// fl-panel: shows when the camera or the microphone is in use ([Panel]
// PrivacyIndicator, on by default): an orange camera or microphone, or both,
// with the programs that use them in the tooltip and the menu. The camera
// comes from PipeWire's video streams (privacy_video) and, for programs that
// open /dev/video* themselves, from /proc: scanned only when inotify reports
// that a camera device was opened or closed, so nothing polls. The microphone
// comes from PipeWire's recording streams (privacy_audio, from the volume code).
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/fl_draw.H>
#include <sys/inotify.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

#include "applets_util.h"
#include "fld/icons.h"
#include "fld/util.h"
#include "panel.h"

namespace {

class Privacy : public fld::FlatButton {
public:
  std::vector<std::string> cam, mic;
  std::string tip;
  Privacy(int x, int y, int w, int h) : fld::FlatButton(x, y, w, h) {}
  void draw() override {
    fld::FlatButton::draw();
    // a rounded orange pill with the icons in white
    Fl_Color orange = fl_rgb_color(0xf0, 0x8c, 0x1a);
    int n = (!cam.empty()) + (!mic.empty());
    int pw = n * 20 + 8, px = x() + (w() - pw) / 2, py = y() + h() / 2 - 11;
    fl_color(orange);
    fl_pie(px, py, 22, 22, 90, 270);
    fl_pie(px + pw - 22, py, 22, 22, 270, 450);
    fl_rectf(px + 11, py, pw - 22, 22);
    int ix = px + 5;
    for (const char *name : {"camera-web", "audio-input-microphone"}) {
      bool on = name[0] == 'c' ? !cam.empty() : !mic.empty();
      if (!on) continue;
      if (Fl_Image *im = fld::symbolic_icon(name, 16, FL_WHITE)) im->draw(ix + 1, py + 3);
      ix += 20;
    }
  }
};

Privacy *g_p;
std::vector<std::string> g_mic;
std::vector<std::string> g_pw_cam;  // from PipeWire
std::vector<std::string> g_proc_cam; // from /proc, at the last scan
int g_inotify = -1, g_dev_wd = -1;

void update() {
  if (!g_p) return;
  auto cam = applets::merge_camera_users(g_pw_cam, g_proc_cam);
  bool changed = cam != g_p->cam || g_mic != g_p->mic;
  g_p->cam = cam;
  g_p->mic = g_mic;
  bool on = !cam.empty() || !g_mic.empty();
  if (changed) {
    g_p->tip.clear();
    if (!cam.empty()) g_p->tip += "The camera is used by " + applets::name_list(cam);
    if (!g_mic.empty()) g_p->tip += std::string(g_p->tip.empty() ? "" : "\n") + "The microphone is used by " + applets::name_list(g_mic);
    g_p->tooltip(g_p->tip.c_str());
    g_p->size((int)((!cam.empty()) + (!g_mic.empty())) * 20 + 20, g_p->h());
    g_p->redraw();
  }
  if (on != (bool)g_p->visible()) {
    if (on) g_p->show();
    else g_p->hide();
    relayout();
  } else if (changed) relayout();
}

void scan(void *) {
  g_proc_cam = applets::camera_users();
  update();
}

// a camera node was opened or closed (or one appeared): look who has it, a
// moment later so that a program that opens it to probe is done
void inotify_cb(int fd, void *) {
  char buf[4096] __attribute__((aligned(__alignof__(inotify_event))));
  bool added = false, camera = false;
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0)
    for (char *p = buf; p < buf + n;) {
      inotify_event *e = (inotify_event *)p;
      if (e->wd != g_dev_wd) camera = true; // a camera node itself
      else if ((e->mask & IN_CREATE) && e->len && strncmp(e->name, "video", 5) == 0) added = true;
      p += sizeof(inotify_event) + e->len;
    }
  if (added)
    for (const std::string &v : applets::video_devices())
      inotify_add_watch(g_inotify, v.c_str(), IN_OPEN | IN_CLOSE);
  if (!camera && !added) return; // another device in /dev
  Fl::remove_timeout(scan);
  Fl::add_timeout(0.3, scan);
}

void watch_cameras() {
  g_inotify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  if (g_inotify < 0) return;
  // a camera plugged in later
  g_dev_wd = inotify_add_watch(g_inotify, "/dev", IN_CREATE);
  for (const std::string &v : applets::video_devices())
    inotify_add_watch(g_inotify, v.c_str(), IN_OPEN | IN_CLOSE);
  Fl::add_fd(g_inotify, FL_READ, inotify_cb);
}

} // namespace

fld::FlatButton *privacy_create(bool on) {
  g_p = new Privacy(0, 0, 40, PANEL_H - 4);
  g_p->color(panel_bg());
  g_p->hover_color(panel_hover());
  g_p->hide();
  panel_context_menu(g_p, [](fld::Menu &m) {
    for (auto &c : g_p->cam) m.item(fld::Menu::text("Camera: " + c), nullptr, FL_MENU_INACTIVE);
    for (auto &c : g_p->mic) m.item(fld::Menu::text("Microphone: " + c), nullptr, FL_MENU_INACTIVE);
    m.divider();
    m.item(_("Privacy Settings…"), [] { fld::spawn({"fl-settings", "privacy"}); });
    m.item(_("Sound Settings…"), [] { fld::spawn({"fl-settings", "sound"}); });
  });
  g_p->callback([](Fl_Widget *, void *) { fld::spawn({"fl-settings", "privacy"}); });
  if (!on) {
    g_p = nullptr; // stays hidden
    return nullptr;
  }
  watch_cameras();
  scan(nullptr);
  return g_p;
}

void privacy_video(const std::vector<std::string> &users, bool running) {
  // a camera source running without a named client: someone, but who?
  std::vector<std::string> u = users;
  if (u.empty() && running && g_proc_cam.empty()) u.push_back("a program");
  if (u == g_pw_cam) return;
  g_pw_cam = u;
  update();
}

void privacy_audio(const std::vector<std::string> &recording) {
  g_mic = recording;
  update();
}
