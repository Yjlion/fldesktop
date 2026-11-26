// Displays: resolution, scale, rotation and position through wlr-output-management
#include "fld/i18n.h"
#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Choice.H>
#include <cstdlib>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Return_Button.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Spinner.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/fl_ask.H>
#include <FL/fl_draw.H>
#include <FL/platform.H>

#include <wayland-client.h>
#ifdef FLD_HAVE_BACKLIGHT
#include "backlight.h"
#endif
#include <FL/Fl_Slider.H>
#include "wlr-output-management-unstable-v1-client-protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

#include "fld/theme.h"
#include "fld/util.h"
#include "fld/widgets.h"
#include "settings_common.h"
#include "settings_page.h"

namespace fld {
namespace {

struct Mode {
  zwlr_output_mode_v1 *obj = nullptr;
  int w = 0, h = 0, refresh = 0; // mHz
  bool preferred = false;
};

struct Head {
  zwlr_output_head_v1 *obj = nullptr;
  std::string name, description, make, model;
  bool enabled = false;
  std::vector<Mode *> modes;
  Mode *current = nullptr;
  int x = 0, y = 0, transform = 0;
  double scale = 1;
};

// what the user configured for one head (also used to revert)
struct HeadConfig {
  std::string name;
  bool enabled;
  Mode *mode;
  int x, y, transform;
  double scale;
};

// a rotation sensor (iio-sensor-proxy reads it): convertibles and tablets
static bool has_accelerometer() {
  for (const std::string &d : list_dir("/sys/bus/iio/devices"))
    if (file_exists("/sys/bus/iio/devices/" + d + "/in_accel_x_raw") ||
        file_exists("/sys/bus/iio/devices/" + d + "/in_accel_x_raw0"))
      return true;
  return false;
}

class DisplaysPage;
DisplaysPage *g_page;

struct Manager {
  zwlr_output_manager_v1 *mgr = nullptr;
  uint32_t serial = 0;
  std::vector<std::unique_ptr<Head>> heads;
  std::vector<std::unique_ptr<Mode>> modes;
  bool done_once = false;
};
Manager M;

void changed(); // UI refresh

// ---- mode events
void mode_size(void *d, zwlr_output_mode_v1 *, int32_t w, int32_t h) { ((Mode *)d)->w = w; ((Mode *)d)->h = h; }
void mode_refresh(void *d, zwlr_output_mode_v1 *, int32_t r) { ((Mode *)d)->refresh = r; }
void mode_preferred(void *d, zwlr_output_mode_v1 *) { ((Mode *)d)->preferred = true; }
void mode_finished(void *d, zwlr_output_mode_v1 *m) {
  Mode *mode = (Mode *)d;
  for (auto &h : M.heads) {
    h->modes.erase(std::remove(h->modes.begin(), h->modes.end(), mode), h->modes.end());
    if (h->current == mode) h->current = nullptr;
  }
  zwlr_output_mode_v1_destroy(m);
  mode->obj = nullptr;
}
const zwlr_output_mode_v1_listener kMode = {mode_size, mode_refresh, mode_preferred, mode_finished};

// ---- head events
void head_name(void *d, zwlr_output_head_v1 *, const char *n) { ((Head *)d)->name = n; }
void head_description(void *d, zwlr_output_head_v1 *, const char *s) { ((Head *)d)->description = s; }
void head_physical_size(void *, zwlr_output_head_v1 *, int32_t, int32_t) {}
void head_mode(void *d, zwlr_output_head_v1 *, zwlr_output_mode_v1 *m) {
  auto mode = std::make_unique<Mode>();
  mode->obj = m;
  zwlr_output_mode_v1_add_listener(m, &kMode, mode.get());
  ((Head *)d)->modes.push_back(mode.get());
  M.modes.push_back(std::move(mode));
}
void head_enabled(void *d, zwlr_output_head_v1 *, int32_t e) { ((Head *)d)->enabled = e; }
void head_current_mode(void *d, zwlr_output_head_v1 *, zwlr_output_mode_v1 *m) {
  Head *h = (Head *)d;
  for (Mode *mode : h->modes) if (mode->obj == m) h->current = mode;
}
void head_position(void *d, zwlr_output_head_v1 *, int32_t x, int32_t y) { ((Head *)d)->x = x; ((Head *)d)->y = y; }
void head_transform(void *d, zwlr_output_head_v1 *, int32_t t) { ((Head *)d)->transform = t; }
void head_scale(void *d, zwlr_output_head_v1 *, wl_fixed_t s) { ((Head *)d)->scale = wl_fixed_to_double(s); }
void head_finished(void *d, zwlr_output_head_v1 *obj) {
  Head *h = (Head *)d;
  zwlr_output_head_v1_destroy(obj);
  M.heads.erase(std::remove_if(M.heads.begin(), M.heads.end(), [h](auto &p) { return p.get() == h; }), M.heads.end());
}
void head_make(void *d, zwlr_output_head_v1 *, const char *s) { ((Head *)d)->make = s; }
void head_model(void *d, zwlr_output_head_v1 *, const char *s) { ((Head *)d)->model = s; }
void head_serial(void *, zwlr_output_head_v1 *, const char *) {}
void head_adaptive_sync(void *, zwlr_output_head_v1 *, uint32_t) {}
const zwlr_output_head_v1_listener kHead = {head_name, head_description, head_physical_size, head_mode,
                                            head_enabled, head_current_mode, head_position, head_transform,
                                            head_scale, head_finished, head_make, head_model, head_serial,
                                            head_adaptive_sync};

// ---- manager events
void mgr_head(void *, zwlr_output_manager_v1 *, zwlr_output_head_v1 *h) {
  auto head = std::make_unique<Head>();
  head->obj = h;
  zwlr_output_head_v1_add_listener(h, &kHead, head.get());
  M.heads.push_back(std::move(head));
}
void mgr_done(void *, zwlr_output_manager_v1 *, uint32_t serial) {
  M.serial = serial;
  M.done_once = true;
  changed();
}
void mgr_finished(void *, zwlr_output_manager_v1 *) { M.mgr = nullptr; }
const zwlr_output_manager_v1_listener kMgr = {mgr_head, mgr_done, mgr_finished};

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  if (!strcmp(iface, zwlr_output_manager_v1_interface.name) && !M.mgr) {
    M.mgr = (zwlr_output_manager_v1 *)wl_registry_bind(reg, name, &zwlr_output_manager_v1_interface,
                                                        std::min<uint32_t>(version, 2));
    zwlr_output_manager_v1_add_listener(M.mgr, &kMgr, nullptr);
  }
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kReg = {reg_global, reg_remove};

// ---- applying a configuration
int g_result; // 0 pending, 1 ok, -1 failed
void cfg_succeeded(void *, zwlr_output_configuration_v1 *c) { g_result = 1; zwlr_output_configuration_v1_destroy(c); }
void cfg_failed(void *, zwlr_output_configuration_v1 *c) { g_result = -1; zwlr_output_configuration_v1_destroy(c); }
void cfg_cancelled(void *, zwlr_output_configuration_v1 *c) { g_result = -1; zwlr_output_configuration_v1_destroy(c); }
const zwlr_output_configuration_v1_listener kCfg = {cfg_succeeded, cfg_failed, cfg_cancelled};

bool apply(const std::vector<HeadConfig> &cfg) {
  if (!M.mgr) return false;
  zwlr_output_configuration_v1 *c = zwlr_output_manager_v1_create_configuration(M.mgr, M.serial);
  zwlr_output_configuration_v1_add_listener(c, &kCfg, nullptr);
  for (auto &hc : cfg) {
    Head *h = nullptr;
    for (auto &x : M.heads) if (x->name == hc.name) h = x.get();
    if (!h) continue;
    if (!hc.enabled) {
      zwlr_output_configuration_v1_disable_head(c, h->obj);
      continue;
    }
    zwlr_output_configuration_head_v1 *ch = zwlr_output_configuration_v1_enable_head(c, h->obj);
    if (hc.mode && hc.mode->obj) zwlr_output_configuration_head_v1_set_mode(ch, hc.mode->obj);
    zwlr_output_configuration_head_v1_set_position(ch, hc.x, hc.y);
    zwlr_output_configuration_head_v1_set_transform(ch, hc.transform);
    zwlr_output_configuration_head_v1_set_scale(ch, wl_fixed_from_double(hc.scale));
  }
  g_result = 0;
  zwlr_output_configuration_v1_apply(c);
  for (int i = 0; i < 200 && g_result == 0; i++) Fl::wait(0.05);
  return g_result == 1;
}

std::vector<HeadConfig> current_config() {
  std::vector<HeadConfig> v;
  for (auto &h : M.heads) v.push_back({h->name, h->enabled, h->current, h->x, h->y, h->transform, h->scale});
  return v;
}

std::string mode_label(const Mode *m) {
  char buf[64];
  snprintf(buf, sizeof buf, "%d × %d @ %.2f Hz%s", m->w, m->h, m->refresh / 1000.0, m->preferred ? "  ★" : "");
  return buf;
}

const double kScales[] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
const char *kRotations[] = {"Normal", "90°", "180°", "270°"};

// drawing of the output arrangement
class Arrangement : public Fl_Widget {
public:
  int selected = 0;
  Arrangement(int x, int y, int w, int h) : Fl_Widget(x, y, w, h) {}
  void draw() override {
    const Palette &p = palette();
    fl_color(fl_color_average(p.bg2, p.fg, 0.92f));
    fl_rectf(x(), y(), w(), h());
    if (M.heads.empty()) return;
    int minx = 1 << 30, miny = 1 << 30, maxx = -(1 << 30), maxy = -(1 << 30);
    auto size_of = [](Head *hd, int &ow, int &oh) {
      ow = hd->current ? hd->current->w : 1920;
      oh = hd->current ? hd->current->h : 1080;
      if (hd->transform % 2) std::swap(ow, oh);
      ow = (int)(ow / hd->scale);
      oh = (int)(oh / hd->scale);
    };
    for (auto &hd : M.heads) {
      int ow, oh;
      size_of(hd.get(), ow, oh);
      minx = std::min(minx, hd->x); miny = std::min(miny, hd->y);
      maxx = std::max(maxx, hd->x + ow); maxy = std::max(maxy, hd->y + oh);
    }
    double s = std::min((w() - 40.0) / std::max(1, maxx - minx), (h() - 40.0) / std::max(1, maxy - miny));
    int ox = x() + (int)((w() - (maxx - minx) * s) / 2), oy = y() + (int)((h() - (maxy - miny) * s) / 2);
    for (size_t i = 0; i < M.heads.size(); i++) {
      Head *hd = M.heads[i].get();
      int ow, oh;
      size_of(hd, ow, oh);
      int rx = ox + (int)((hd->x - minx) * s), ry = oy + (int)((hd->y - miny) * s);
      int rw = (int)(ow * s), rh = (int)(oh * s);
      fl_color(!hd->enabled ? p.fg_dim : (int)i == selected ? p.accent : fl_color_average(p.accent, p.bg2, 0.5f));
      fl_rectf(rx + 2, ry + 2, rw - 4, rh - 4);
      fl_color(p.accent_fg);
      fl_font(FL_HELVETICA_BOLD, 13);
      fl_draw(escape_at(hd->name).c_str(), rx, ry, rw, rh, FL_ALIGN_CENTER | FL_ALIGN_CLIP);
    }
  }
};

class DisplaysPage : public Fl_Group {
public:
  Fl_Hold_Browser *list;
  Arrangement *arr;
  Fl_Check_Button *enabled;
  Fl_Choice *mode, *scale, *rotation;
  Fl_Spinner *px, *py;
  Fl_Button *apply_btn;
  Fl_Box *status;
  int sel = 0;
  bool dirty = false;
  std::vector<HeadConfig> edited;
  // night light ([NightLight] in desktop.ini, applied by fl-nightlight)
  Fl_Check_Button *night;
  Fl_Choice *night_schedule;
  Fl_Input *night_from, *night_to;
  Fl_Value_Slider *night_temp;

  DisplaysPage(int X, int Y, int W, int H) : Fl_Group(X, Y, W, H) {
    g_page = this;
    int cy;
    Fl_Group *g = page_group(X, Y, W, H, "Displays", cy);
    resizable(g);
    int lw = std::min(W - 48, 680);
    arr = new Arrangement(X + 24, cy, lw, 170);
    list = new Fl_Hold_Browser(X + 24, cy + 180, 200, 210);
    list->textsize(13);
    list->callback([](Fl_Widget *, void *d) {
      DisplaysPage *p = (DisplaysPage *)d;
      if (p->list->value() > 0) p->sel = p->list->value() - 1;
      p->arr->selected = p->sel;
      p->arr->redraw();
      p->show_head();
    }, this);
    Form f(g, X + 24 + 210, cy + 180, lw - 210, 110);
    int wx, wy, ww;
    f.row("", 26, wx, wy, ww);
    enabled = new fld::CheckButton(wx, wy, ww, 26, _("Enabled"));
    enabled->callback([](Fl_Widget *, void *d) { ((DisplaysPage *)d)->edit(); }, this);
    f.row(_("Resolution:"), 28, wx, wy, ww);
    mode = new Fl_Choice(wx, wy, ww, 28);
    mode->callback([](Fl_Widget *, void *d) { ((DisplaysPage *)d)->edit(); }, this);
    f.row(_("Scale:"), 28, wx, wy, ww);
    scale = new Fl_Choice(wx, wy, 140, 28);
    for (double s : kScales) scale->add((std::to_string((int)lround(s * 100)) + "\\%").c_str());
    scale->callback([](Fl_Widget *, void *d) { ((DisplaysPage *)d)->edit(); }, this);
    f.row(_("Rotation:"), 28, wx, wy, ww);
    rotation = new Fl_Choice(wx, wy, 140, 28);
    for (auto r : kRotations) rotation->add(r);
    rotation->callback([](Fl_Widget *, void *d) { ((DisplaysPage *)d)->edit(); }, this);
    f.row(_("Position:"), 28, wx, wy, ww);
    px = new Fl_Spinner(wx, wy, 100, 28);
    py = new Fl_Spinner(wx + 110, wy, 100, 28);
    for (Fl_Spinner *s : {px, py}) {
      s->range(-32768, 32768);
      s->step(10);
      s->callback([](Fl_Widget *, void *d) { ((DisplaysPage *)d)->edit(); }, this);
    }
    apply_btn = new Fl_Button(X + 24 + lw - 120, cy + 400, 120, 32, _("Apply"));
    apply_btn->callback([](Fl_Widget *, void *d) { ((DisplaysPage *)d)->do_apply(); }, this);
    apply_btn->deactivate();
    status = new Fl_Box(X + 24, cy + 400, lw - 130, 32);
    status->align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT | FL_ALIGN_WRAP);
    status->labelsize(12);
    status->labelcolor(palette().fg_dim);

    int ny = cy + 450;
    if (has_accelerometer()) {
      // convertibles and tablets: fl-autorotate follows the accelerometer
      Fl_Check_Button *rot = new fld::CheckButton(X + 24, ny, lw, 26, _("Rotate the built-in screen with the device"));
      rot->tooltip(_("For convertibles and tablets with a rotation sensor (iio-sensor-proxy)"));
      ConfigFile d(desktop_ini_path());
      rot->value(d.ini.get_bool("Display", "AutoRotate", true));
      rot->callback([](Fl_Widget *w, void *) {
        ConfigFile d(desktop_ini_path());
        d.ini.set_bool("Display", "AutoRotate", ((Fl_Check_Button *)w)->value());
        d.save();
      });
      ny += 34;
    }
#ifdef FLD_HAVE_BACKLIGHT
    // the built-in screen's backlight (laptops)
    int pct;
    if (bl::screen_get(pct)) {
      Form bf(g, X + 24, ny, lw);
      bf.heading(_("Brightness"));
      bf.row(_("Built-in screen:"), 26, wx, wy, ww);
      Fl_Slider *b = new Fl_Slider(wx, wy, std::min(ww, 320), 26);
      b->type(FL_HOR_NICE_SLIDER);
      b->bounds(1, 100);
      b->step(1);
      b->value(pct);
      b->when(FL_WHEN_CHANGED);
      b->callback([](Fl_Widget *w, void *) {
        std::string err;
        if (!bl::screen_set((int)((Fl_Slider *)w)->value(), err)) fprintf(stderr, "fl-settings: %s\n", err.c_str());
      });
      bf.note(_("The brightness keys change it too."));
      ny = bf.y() + 16;
    }
#endif
    Form nf(g, X + 24, ny, lw);
    nf.heading(_("Night Light"));
    ConfigFile desk(desktop_ini_path());
    const std::string N = "NightLight";
    auto ncb = [](Fl_Widget *, void *d) { ((DisplaysPage *)d)->save_night(); };
    nf.row("", 26, wx, wy, ww);
    night = new fld::CheckButton(wx, wy, ww, 26, _("Warmer colours at night"));
    night->value(desk.ini.get_bool(N, "Enabled", false));
    night->callback(ncb, this);
    nf.row(_("When:"), 28, wx, wy, ww);
    night_schedule = new Fl_Choice(wx, wy, 220, 28);
    night_schedule->add(_("Sunset to sunrise|Custom hours|All the time"));
    std::string sch = desk.ini.get(N, "Schedule", "sunset");
    night_schedule->value(sch == "custom" ? 1 : sch == "always" ? 2 : 0);
    night_schedule->callback(ncb, this);
    nf.row(_("From:"), 28, wx, wy, ww);
    night_from = new Fl_Input(wx, wy, 70, 28);
    night_from->value(desk.ini.get(N, "From", "20:00").c_str());
    Fl_Box *to_label = new Fl_Box(wx + 76, wy, 30, 28, "to");
    to_label->align(FL_ALIGN_INSIDE | FL_ALIGN_CENTER);
    night_to = new Fl_Input(wx + 112, wy, 70, 28);
    night_to->value(desk.ini.get(N, "To", "07:00").c_str());
    for (Fl_Input *in : {night_from, night_to}) {
      in->when(FL_WHEN_RELEASE | FL_WHEN_ENTER_KEY);
      in->callback(ncb, this);
    }
    nf.row(_("Warmth:"), 26, wx, wy, ww);
    night_temp = new Fl_Value_Slider(wx, wy, std::min(ww, 320), 26);
    night_temp->type(FL_HOR_NICE_SLIDER);
    // left: barely warmer, right: very warm; the value shown is in kelvin
    night_temp->bounds(6000, 2500);
    night_temp->step(100);
    night_temp->value(desk.ini.get_int(N, "Temperature", 4000));
    night_temp->when(FL_WHEN_RELEASE);
    night_temp->callback(ncb, this);
    nf.note(_("Sunset and sunrise are worked out from the time zone (Date & Time)."));
    update_night();

    // how fl-wm draws the screen (wm.ini [behaviour] renderer=, read at login)
    Form rf(g, X + 24, nf.y() + 16, lw);
    rf.heading(_("Graphics"));
    rf.row(_("Drawing:"), 28, wx, wy, ww);
    Fl_Choice *rend = new Fl_Choice(wx, wy, std::min(ww, 300), 28);
    rend->add(_("Automatic|Graphics card (OpenGL ES)|Graphics card (Vulkan, experimental)|Software (least memory)"));
    static const char *const kRenderers[] = {"auto", "gles2", "vulkan", "pixman"};
    {
      ConfigFile wm(wm_ini_path());
      std::string r = wm.ini.get("behaviour", "renderer", "auto");
      int v = 0;
      for (int i = 0; i < 4; i++)
        if (r == kRenderers[i]) v = i;
      rend->value(v);
    }
    rend->callback([](Fl_Widget *w, void *) {
      ConfigFile wm(wm_ini_path());
      wm.ini.set("behaviour", "renderer", kRenderers[((Fl_Choice *)w)->value()]);
      save_wm(wm);
    });
    static std::string rnote;
    const char *cur = getenv("FLWM_RENDERER");
    rnote = std::string("Takes effect at the next login") + (cur && *cur ? std::string(" (now: ") + cur + ")" : "") +
            ". Software drawing needs about 50 MB less memory on older graphics cards, but more processor time"
            " while windows move.";
    rf.note(rnote.c_str(), 3);
    g->end();
    end();
    if (wl_display *dpy = fl_wl_display()) {
      wl_registry *reg = wl_display_get_registry(dpy);
      wl_registry_add_listener(reg, &kReg, nullptr);
      wl_display_roundtrip(dpy);
      wl_display_roundtrip(dpy);
    }
    if (!M.mgr) status->label(_("The compositor does not support output management (wlr-output-management)."));
    refresh();
  }

  // One screen has nothing to arrange: the arrangement, the list of screens
  // and the position go, and the rest of the page moves up and left.
  bool single_ = false;
  void set_single(bool single) {
    if (single == single_) return;
    single_ = single;
    int dx = single ? -210 : 210, dy = single ? -180 : 180;
    int top = arr->y(), form_x = list->x() + 210;
    Fl_Group *g = arr->parent();
    for (int i = 0; i < g->children(); i++) {
      Fl_Widget *w = g->child(i);
      if (w == arr || w == list) continue;
      // where it is with the arrangement shown
      int oy = single ? w->y() : w->y() + 180;
      if (oy < top + 170) continue;
      bool in_form = oy < top + 400 && w->x() + (single ? 0 : 210) >= form_x - 200;
      w->position(w->x() + (in_form ? dx : 0), w->y() + dy);
      // the position row (its label too) is for arranging
      if (w == px || w == py || (w->label() && !strcmp(w->label(), _("Position:")))) single ? w->hide() : w->show();
    }
    single ? arr->hide() : arr->show();
    single ? list->hide() : list->show();
    g->init_sizes();
    redraw();
  }

  void refresh() {
    if (dirty) return; // do not overwrite unsaved edits
    set_single(M.heads.size() == 1);
    edited = current_config();
    list->clear();
    for (auto &h : M.heads) {
      std::string l = h->name + (h->enabled ? "" : " (off)");
      list->add(escape_at(l).c_str());
    }
    if (sel >= (int)M.heads.size()) sel = 0;
    list->value(sel + 1);
    arr->selected = sel;
    arr->redraw();
    show_head();
  }

  void show_head() {
    if (sel >= (int)M.heads.size()) return;
    Head *h = M.heads[sel].get();
    HeadConfig &c = edited[sel];
    enabled->value(c.enabled);
    mode->clear();
    int mi = 0;
    for (size_t i = 0; i < h->modes.size(); i++) {
      mode->add(escape_at(mode_label(h->modes[i])).c_str());
      if (h->modes[i] == c.mode) mi = (int)i;
    }
    mode->value(h->modes.empty() ? -1 : mi);
    int si = 0;
    for (int i = 0; i < 7; i++) if (std::fabs(kScales[i] - c.scale) < 0.01) si = i;
    scale->value(si);
    rotation->value(c.transform & 3);
    px->value(c.x);
    py->value(c.y);
    static std::string desc;
    desc = h->description.empty() ? h->make + " " + h->model : h->description;
    list->tooltip(desc.c_str());
  }

  void edit() {
    if (sel >= (int)M.heads.size()) return;
    Head *h = M.heads[sel].get();
    HeadConfig &c = edited[sel];
    c.enabled = enabled->value();
    int mi = mode->value();
    if (mi >= 0 && mi < (int)h->modes.size()) c.mode = h->modes[mi];
    c.scale = kScales[std::max(0, scale->value())];
    c.transform = std::max(0, rotation->value());
    c.x = (int)px->value();
    c.y = (int)py->value();
    dirty = true;
    apply_btn->activate();
  }

  // asks to keep the new configuration; reverts after 15 seconds
  bool confirm() {
    Fl_Double_Window w(400, 130, "Keep Display Settings?");
    Fl_Box *msg = new Fl_Box(16, 12, 368, 60);
    msg->align(FL_ALIGN_INSIDE | FL_ALIGN_WRAP | FL_ALIGN_LEFT);
    Fl_Button *revert = new Fl_Button(176, 86, 100, 30, _("Revert"));
    Fl_Return_Button *keep = new Fl_Return_Button(284, 86, 100, 30, _("Keep"));
    static int result;
    result = 0;
    static std::string text;
    text = "Do you want to keep these display settings? They are reverted in 15 s.";
    msg->label(text.c_str());
    revert->callback([](Fl_Widget *b, void *) { b->window()->hide(); });
    keep->callback([](Fl_Widget *b, void *) { result = 1; b->window()->hide(); });
    w.end();
    w.set_modal();
    w.show();
    for (int t = 150; t > 0 && w.shown(); t--) {
      Fl::wait(0.1);
      if (t % 10 == 0) {
        text = "Do you want to keep these display settings? They are reverted in " + std::to_string(t / 10) + " s.";
        msg->label(text.c_str());
        msg->redraw();
      }
    }
    w.hide();
    return result == 1;
  }

  void update_night() {
    bool on = night->value();
    bool custom = night_schedule->value() == 1;
    for (Fl_Widget *w : {(Fl_Widget *)night_schedule, (Fl_Widget *)night_temp})
      on ? w->activate() : w->deactivate();
    for (Fl_Widget *w : {(Fl_Widget *)night_from, (Fl_Widget *)night_to})
      on && custom ? w->activate() : w->deactivate();
  }

  static bool valid_hhmm(const char *s) {
    int h, m;
    char extra;
    return sscanf(s, "%d:%d%c", &h, &m, &extra) == 2 && h >= 0 && h < 24 && m >= 0 && m < 60;
  }

  void save_night() {
    update_night();
    ConfigFile desk(desktop_ini_path());
    const std::string N = "NightLight";
    bool was_on = desk.ini.get_bool(N, "Enabled", false);
    desk.ini.set_bool(N, "Enabled", night->value());
    const char *sch[] = {"sunset", "custom", "always"};
    desk.ini.set(N, "Schedule", sch[std::max(0, night_schedule->value())]);
    if (valid_hhmm(night_from->value())) desk.ini.set(N, "From", night_from->value());
    if (valid_hhmm(night_to->value())) desk.ini.set(N, "To", night_to->value());
    desk.ini.set_int(N, "Temperature", (int)night_temp->value());
    desk.save();
    // fl-nightlight starts with the session; start it now if it is not running
    if (night->value() && !was_on && !which("fl-nightlight").empty())
      spawn({"sh", "-c", "pgrep -u \"$(id -u)\" -x fl-nightlight >/dev/null || exec fl-nightlight"});
  }

  void save_config() {
    ConfigFile wm(wm_ini_path());
    for (auto &c : edited) {
      std::string g = "output:" + c.name;
      wm.ini.set_bool(g, "enabled", c.enabled);
      if (c.mode) {
        char buf[64];
        snprintf(buf, sizeof buf, "%dx%d@%.3f", c.mode->w, c.mode->h, c.mode->refresh / 1000.0);
        wm.ini.set(g, "mode", buf);
      }
      char sc[16];
      snprintf(sc, sizeof sc, "%.2f", c.scale);
      wm.ini.set(g, "scale", sc);
      wm.ini.set(g, "position", std::to_string(c.x) + "," + std::to_string(c.y));
      const char *tr[] = {"normal", "90", "180", "270"};
      wm.ini.set(g, "transform", tr[c.transform & 3]);
    }
    wm.save(); // fl-wm already runs with this configuration
  }

  void do_apply() {
    std::vector<HeadConfig> before = current_config();
    bool any_enabled = false;
    for (auto &c : edited) if (c.enabled) any_enabled = true;
    if (!any_enabled) { fl_alert("%s", _("At least one display must stay enabled.")); return; }
    if (!apply(edited)) {
      fl_alert("%s", _("The display configuration could not be applied."));
      dirty = false;
      refresh();
      return;
    }
    if (confirm()) {
      save_config();
      status->label(_("Saved."));
    } else {
      apply(before);
      status->label(_("Reverted."));
    }
    dirty = false;
    apply_btn->deactivate();
    // the manager sends a new configuration (done) which refreshes the page
    Fl::wait(0.2);
    refresh();
  }
};

void changed() {
  if (g_page) g_page->refresh();
}

Fl_Group *create(int x, int y, int w, int h) { return new DisplaysPage(x, y, w, h); }

} // namespace

PageInfo page_displays() {
  return {"displays", _("Displays"), "video-display", "Hardware",
          _("display monitor screen resolution scale hidpi rotation position output refresh night light warm colour color temperature brightness backlight"), create};
}

} // namespace fld
