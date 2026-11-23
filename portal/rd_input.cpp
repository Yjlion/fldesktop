#include "rd_input.h"

#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstring>
#include <vector>

#include "fld/ini.h"
#include "fld/util.h"
#include "virtual-keyboard-unstable-v1-client-protocol.h"
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

namespace portal::rd {

namespace {

struct Output {
  wl_output *out = nullptr;
  zxdg_output_v1 *xo = nullptr;
  std::string name;
  int x = 0, y = 0, w = 0, h = 0;
};

struct State {
  wl_display *dpy = nullptr;
  wl_registry *reg = nullptr;
  wl_seat *seat = nullptr;
  zwlr_virtual_pointer_manager_v1 *pmgr = nullptr;
  zwp_virtual_keyboard_manager_v1 *kmgr = nullptr;
  zxdg_output_manager_v1 *xomgr = nullptr;
  zwlr_virtual_pointer_v1 *ptr = nullptr;
  zwp_virtual_keyboard_v1 *kbd = nullptr;
  std::vector<Output *> outputs;
  xkb_context *ctx = nullptr;
  xkb_keymap *keymap = nullptr;
  xkb_state *xstate = nullptr;
} S;

uint32_t now_ms() {
  return (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void xo_position(void *d, zxdg_output_v1 *, int32_t x, int32_t y) {
  ((Output *)d)->x = x;
  ((Output *)d)->y = y;
}
void xo_size(void *d, zxdg_output_v1 *, int32_t w, int32_t h) {
  ((Output *)d)->w = w;
  ((Output *)d)->h = h;
}
void xo_done(void *, zxdg_output_v1 *) {}
void xo_name(void *d, zxdg_output_v1 *, const char *n) { ((Output *)d)->name = n; }
void xo_desc(void *, zxdg_output_v1 *, const char *) {}
const zxdg_output_v1_listener kXo = {xo_position, xo_size, xo_done, xo_name, xo_desc};

void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  std::string i = iface;
  if (i == wl_seat_interface.name && !S.seat) S.seat = (wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
  else if (i == zwlr_virtual_pointer_manager_v1_interface.name)
    S.pmgr = (zwlr_virtual_pointer_manager_v1 *)wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
  else if (i == zwp_virtual_keyboard_manager_v1_interface.name)
    S.kmgr = (zwp_virtual_keyboard_manager_v1 *)wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
  else if (i == zxdg_output_manager_v1_interface.name)
    S.xomgr = (zxdg_output_manager_v1 *)wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface,
                                                          std::min<uint32_t>(version, 2));
  else if (i == wl_output_interface.name) {
    Output *o = new Output;
    o->out = (wl_output *)wl_registry_bind(reg, name, &wl_output_interface, 1);
    S.outputs.push_back(o);
  }
}
void reg_remove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kReg = {reg_global, reg_remove};

// the keymap of the session (wm.ini [keyboard]) for the virtual keyboard
bool load_keymap() {
  fld::Ini wm;
  wm.load(fld::wm_config_path());
  std::string layout = wm.get("keyboard", "layout", ""), variant = wm.get("keyboard", "variant", ""),
              options = wm.get("keyboard", "options", ""), model = wm.get("keyboard", "model", "");
  xkb_rule_names names{};
  names.layout = layout.empty() ? nullptr : layout.c_str();
  names.variant = variant.empty() ? nullptr : variant.c_str();
  names.options = options.empty() ? nullptr : options.c_str();
  names.model = model.empty() ? nullptr : model.c_str();
  S.ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  if (!S.ctx) return false;
  S.keymap = xkb_keymap_new_from_names(S.ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!S.keymap) return false;
  S.xstate = xkb_state_new(S.keymap);
  char *text = xkb_keymap_get_as_string(S.keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  if (!text) return false;
  size_t size = strlen(text) + 1;
  int fd = memfd_create("fl-portal-keymap", MFD_CLOEXEC);
  bool ok = fd >= 0 && write(fd, text, size) == (ssize_t)size;
  free(text);
  if (ok) zwp_virtual_keyboard_v1_keymap(S.kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
  if (fd >= 0) ::close(fd);
  return ok;
}

void send_modifiers() {
  if (!S.xstate) return;
  zwp_virtual_keyboard_v1_modifiers(S.kbd, xkb_state_serialize_mods(S.xstate, XKB_STATE_MODS_DEPRESSED),
                                    xkb_state_serialize_mods(S.xstate, XKB_STATE_MODS_LATCHED),
                                    xkb_state_serialize_mods(S.xstate, XKB_STATE_MODS_LOCKED),
                                    xkb_state_serialize_layout(S.xstate, XKB_STATE_LAYOUT_EFFECTIVE));
}

void flush() { wl_display_flush(S.dpy); }

} // namespace

bool is_open() { return S.dpy && S.ptr && S.kbd; }

bool open(std::string &err) {
  if (is_open()) return true;
  close();
  S.dpy = wl_display_connect(nullptr);
  if (!S.dpy) {
    err = "no Wayland display";
    return false;
  }
  S.reg = wl_display_get_registry(S.dpy);
  wl_registry_add_listener(S.reg, &kReg, nullptr);
  wl_display_roundtrip(S.dpy);
  if (!S.pmgr || !S.kmgr || !S.seat) {
    err = "the compositor lacks virtual pointer or virtual keyboard support";
    close();
    return false;
  }
  if (S.xomgr)
    for (Output *o : S.outputs) {
      o->xo = zxdg_output_manager_v1_get_xdg_output(S.xomgr, o->out);
      zxdg_output_v1_add_listener(o->xo, &kXo, o);
    }
  wl_display_roundtrip(S.dpy);
  S.ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(S.pmgr, S.seat);
  S.kbd = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(S.kmgr, S.seat);
  if (!load_keymap()) {
    err = "cannot make a keymap";
    close();
    return false;
  }
  wl_display_roundtrip(S.dpy);
  return true;
}

void close() {
  if (S.ptr) zwlr_virtual_pointer_v1_destroy(S.ptr);
  if (S.kbd) zwp_virtual_keyboard_v1_destroy(S.kbd);
  for (Output *o : S.outputs) {
    if (o->xo) zxdg_output_v1_destroy(o->xo);
    wl_output_destroy(o->out);
    delete o;
  }
  S.outputs.clear();
  if (S.xstate) xkb_state_unref(S.xstate);
  if (S.keymap) xkb_keymap_unref(S.keymap);
  if (S.ctx) xkb_context_unref(S.ctx);
  if (S.dpy) wl_display_disconnect(S.dpy);
  S = State();
}

void motion(double dx, double dy) {
  if (!is_open()) return;
  zwlr_virtual_pointer_v1_motion(S.ptr, now_ms(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
  zwlr_virtual_pointer_v1_frame(S.ptr);
  flush();
}

void motion_absolute(const std::string &output, double x, double y) {
  if (!is_open() || S.outputs.empty()) return;
  // the layout's bounding box: absolute motion is relative to it
  int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
  const Output *target = nullptr;
  for (const Output *o : S.outputs) {
    if (o->w <= 0 || o->h <= 0) continue;
    minx = std::min(minx, o->x);
    miny = std::min(miny, o->y);
    maxx = std::max(maxx, o->x + o->w);
    maxy = std::max(maxy, o->y + o->h);
    if (o->name == output || (!target && output.empty())) target = o;
  }
  if (!target || maxx <= minx || maxy <= miny) return;
  double lx = target->x + std::max(0.0, std::min(x, (double)target->w - 1)) - minx;
  double ly = target->y + std::max(0.0, std::min(y, (double)target->h - 1)) - miny;
  zwlr_virtual_pointer_v1_motion_absolute(S.ptr, now_ms(), (uint32_t)lx, (uint32_t)ly, (uint32_t)(maxx - minx),
                                          (uint32_t)(maxy - miny));
  zwlr_virtual_pointer_v1_frame(S.ptr);
  flush();
}

void button(int b, bool pressed) {
  if (!is_open()) return;
  zwlr_virtual_pointer_v1_button(S.ptr, now_ms(), (uint32_t)b,
                                 pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
  zwlr_virtual_pointer_v1_frame(S.ptr);
  flush();
}

void axis(double dx, double dy, bool finish) {
  if (!is_open()) return;
  uint32_t t = now_ms();
  zwlr_virtual_pointer_v1_axis_source(S.ptr, WL_POINTER_AXIS_SOURCE_FINGER);
  if (dy != 0) zwlr_virtual_pointer_v1_axis(S.ptr, t, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_double(dy));
  if (dx != 0) zwlr_virtual_pointer_v1_axis(S.ptr, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL, wl_fixed_from_double(dx));
  if (finish) {
    zwlr_virtual_pointer_v1_axis_stop(S.ptr, t, WL_POINTER_AXIS_VERTICAL_SCROLL);
    zwlr_virtual_pointer_v1_axis_stop(S.ptr, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
  }
  zwlr_virtual_pointer_v1_frame(S.ptr);
  flush();
}

void axis_discrete(int ax, int steps) {
  if (!is_open() || steps == 0) return;
  uint32_t a = ax == 1 ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL;
  zwlr_virtual_pointer_v1_axis_source(S.ptr, WL_POINTER_AXIS_SOURCE_WHEEL);
  zwlr_virtual_pointer_v1_axis_discrete(S.ptr, now_ms(), a, wl_fixed_from_int(15 * steps), steps);
  zwlr_virtual_pointer_v1_frame(S.ptr);
  flush();
}

void key(int code, bool pressed) {
  if (!is_open() || code < 0) return;
  zwp_virtual_keyboard_v1_key(S.kbd, now_ms(), (uint32_t)code,
                              pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
  if (S.xstate) {
    xkb_state_update_key(S.xstate, (xkb_keycode_t)code + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
    send_modifiers();
  }
  flush();
}

bool keysym(uint32_t sym, bool pressed) {
  if (!is_open() || !S.keymap) return false;
  // the first key (and level) of the first layout that makes the keysym;
  // Shift is pressed around it for level 2
  xkb_keycode_t found = 0;
  xkb_level_index_t level = 0;
  for (xkb_keycode_t kc = xkb_keymap_min_keycode(S.keymap); kc <= xkb_keymap_max_keycode(S.keymap) && !found; kc++) {
    for (xkb_level_index_t lv = 0; lv < 2 && !found; lv++) {
      const xkb_keysym_t *syms;
      int n = xkb_keymap_key_get_syms_by_level(S.keymap, kc, 0, lv, &syms);
      for (int i = 0; i < n; i++)
        if (syms[i] == sym) {
          found = kc;
          level = lv;
        }
    }
  }
  if (!found) return false;
  const int kShift = 42; // KEY_LEFTSHIFT
  if (level == 1 && pressed) key(kShift, true);
  key((int)found - 8, pressed);
  if (level == 1 && !pressed) key(kShift, false);
  return true;
}

} // namespace portal::rd
