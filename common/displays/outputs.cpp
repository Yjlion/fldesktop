#include "outputs.h"

#include <FL/Fl.H>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <wayland-client.h>

#include "fld/ini.h"
#include "wlr-output-management-unstable-v1-client-protocol.h"

namespace fld {
namespace outputs {

const double kScales[7] = {1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
std::function<void()> on_changed;

namespace {

struct Manager {
  zwlr_output_manager_v1 *mgr = nullptr;
  uint32_t serial = 0;
  std::vector<std::unique_ptr<Head>> heads;
  std::vector<std::unique_ptr<Mode>> modes;
};
Manager M;

void mode_size(void *d, zwlr_output_mode_v1 *, int32_t w, int32_t h) {
  ((Mode *)d)->w = w;
  ((Mode *)d)->h = h;
}
void mode_refresh(void *d, zwlr_output_mode_v1 *, int32_t r) { ((Mode *)d)->refresh = r; }
void mode_preferred(void *d, zwlr_output_mode_v1 *) { ((Mode *)d)->preferred = true; }
void mode_finished(void *d, zwlr_output_mode_v1 *m) {
  Mode *mode = (Mode *)d;
  for (auto &h : M.heads) {
    h->modes.erase(std::remove(h->modes.begin(), h->modes.end(), mode), h->modes.end());
    if (h->current == mode) h->current = nullptr;
  }
  mode->obj = nullptr;
  zwlr_output_mode_v1_destroy(m);
}
const zwlr_output_mode_v1_listener kMode = {mode_size, mode_refresh, mode_preferred, mode_finished};

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
  for (Mode *mode : h->modes)
    if (mode->obj == m) h->current = mode;
}
void head_position(void *d, zwlr_output_head_v1 *, int32_t x, int32_t y) {
  ((Head *)d)->x = x;
  ((Head *)d)->y = y;
}
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
const zwlr_output_head_v1_listener kHead = {head_name,       head_description, head_physical_size, head_mode,
                                            head_enabled,    head_current_mode, head_position,     head_transform,
                                            head_scale,      head_finished,    head_make,          head_model,
                                            head_serial,     head_adaptive_sync};

void mgr_head(void *, zwlr_output_manager_v1 *, zwlr_output_head_v1 *h) {
  auto head = std::make_unique<Head>();
  head->obj = h;
  zwlr_output_head_v1_add_listener(h, &kHead, head.get());
  M.heads.push_back(std::move(head));
}
void mgr_done(void *, zwlr_output_manager_v1 *, uint32_t serial) {
  M.serial = serial;
  if (on_changed) on_changed();
}
void mgr_finished(void *, zwlr_output_manager_v1 *) { M.mgr = nullptr; }
const zwlr_output_manager_v1_listener kMgr = {mgr_head, mgr_done, mgr_finished};

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  if (!strcmp(iface, zwlr_output_manager_v1_interface.name) && !M.mgr) {
    M.mgr = (zwlr_output_manager_v1 *)wl_registry_bind(reg, name, &zwlr_output_manager_v1_interface,
                                                         version < 2 ? version : 2);
    zwlr_output_manager_v1_add_listener(M.mgr, &kMgr, nullptr);
  }
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kReg = {reg_global, reg_remove};

int g_result;  // 0 pending, 1 ok, -1 failed
void cfg_succeeded(void *, zwlr_output_configuration_v1 *c) {
  g_result = 1;
  zwlr_output_configuration_v1_destroy(c);
}
void cfg_failed(void *, zwlr_output_configuration_v1 *c) {
  g_result = -1;
  zwlr_output_configuration_v1_destroy(c);
}
const zwlr_output_configuration_v1_listener kCfg = {cfg_succeeded, cfg_failed, cfg_failed};

}  // namespace

bool connect(wl_display *dpy) {
  if (M.mgr) return true;
  if (!dpy) return false;
  wl_registry *reg = wl_display_get_registry(dpy);
  wl_registry_add_listener(reg, &kReg, nullptr);
  wl_display_roundtrip(dpy);  // the manager
  wl_display_roundtrip(dpy);  // its heads and modes
  return M.mgr != nullptr;
}

bool available() { return M.mgr != nullptr; }
const std::vector<std::unique_ptr<Head>> &heads() { return M.heads; }

std::vector<HeadConfig> current_config() {
  std::vector<HeadConfig> v;
  for (auto &h : M.heads) v.push_back({h->name, h->enabled, h->current, h->x, h->y, h->transform, h->scale});
  return v;
}

bool apply(const std::vector<HeadConfig> &cfg) {
  if (!M.mgr) return false;
  zwlr_output_configuration_v1 *c = zwlr_output_manager_v1_create_configuration(M.mgr, M.serial);
  zwlr_output_configuration_v1_add_listener(c, &kCfg, nullptr);
  for (auto &hc : cfg) {
    Head *h = nullptr;
    for (auto &x : M.heads)
      if (x->name == hc.name) h = x.get();
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

std::string mode_label(const Mode &m) {
  char buf[64];
  snprintf(buf, sizeof buf, "%d × %d @ %.2f Hz%s", m.w, m.h, m.refresh / 1000.0, m.preferred ? "  ★" : "");
  return buf;
}

void write_wm_config(Ini &wm, const std::vector<HeadConfig> &cfg) {
  for (auto &c : cfg) {
    std::string g = "output:" + c.name;
    wm.set_bool(g, "enabled", c.enabled);
    if (c.mode) {
      char buf[64];
      snprintf(buf, sizeof buf, "%dx%d@%.3f", c.mode->w, c.mode->h, c.mode->refresh / 1000.0);
      wm.set(g, "mode", buf);
    }
    char sc[16];
    snprintf(sc, sizeof sc, "%.2f", c.scale);
    wm.set(g, "scale", sc);
    wm.set(g, "position", std::to_string(c.x) + "," + std::to_string(c.y));
    const char *tr[] = {"normal", "90", "180", "270"};
    wm.set(g, "transform", tr[c.transform & 3]);
  }
}

}  // namespace outputs
}  // namespace fld
