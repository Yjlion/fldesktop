// fl-panel: shows when the camera or the microphone is in use ([Panel]
// PrivacyIndicator, on by default): an orange camera or microphone, or both,
// with the programs that use them in the tooltip and the menu. The camera is
// found in /proc (open /dev/video*), the microphone from PipeWire's recording
// streams (privacy_audio, from the volume code).
#include <FL/fl_draw.H>

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

void update() {
  if (!g_p) return;
  auto cam = applets::camera_users();
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

void tick(void *) {
  update();
  Fl::repeat_timeout(3.0, tick);
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
    m.item("Privacy Settings…", [] { fld::spawn({"fl-settings", "privacy"}); });
    m.item("Sound Settings…", [] { fld::spawn({"fl-settings", "sound"}); });
  });
  g_p->callback([](Fl_Widget *, void *) { fld::spawn({"fl-settings", "privacy"}); });
  if (!on) {
    g_p = nullptr; // stays hidden
    return nullptr;
  }
  update();
  Fl::add_timeout(3.0, tick);
  return g_p;
}

void privacy_audio(const std::vector<std::string> &recording) {
  g_mic = recording;
  update();
}
