// fl-osk: on-screen keyboard for fldesktop
//
// A borderless window titled "fldesktop.osk": fl-wm docks it at the bottom
// of the screen and never gives it the keyboard focus. Keys are sent with the
// zwp_virtual_keyboard_v1 protocol. Starting fl-osk again closes it.
#include <FL/Fl.H>
#include <FL/Fl_Double_Window.H>
#include <FL/platform.H>

#include <wayland-client.h>
#include "virtual-keyboard-unstable-v1-client-protocol.h"

#include <csignal>
#include <cstring>
#include <ctime>
#include <sys/mman.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon.h>

#include "fld/ini.h"
#include "fld/osk.h"
#include "fld/theme.h"
#include "fld/util.h"
#include "vkeymap.h"

static wl_seat *g_seat;
static zwp_virtual_keyboard_manager_v1 *g_vk_mgr;

static void reg_global(void *, wl_registry *reg, uint32_t name, const char *iface, uint32_t version) {
  if (!strcmp(iface, wl_seat_interface.name) && !g_seat)
    g_seat = (wl_seat *)wl_registry_bind(reg, name, &wl_seat_interface, 1);
  else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name))
    g_vk_mgr = (zwp_virtual_keyboard_manager_v1 *)wl_registry_bind(reg, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
}
static void reg_remove(void *, wl_registry *, uint32_t) {}
static const wl_registry_listener kReg = {reg_global, reg_remove};

class VirtualKeyboardSink : public fld::OskSink {
public:
  bool init() {
    wl_display *dpy = fl_wl_display();
    if (!dpy) return false;
    wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &kReg, nullptr);
    wl_display_roundtrip(dpy);
    if (!g_vk_mgr || !g_seat) return false;
    vk_ = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(g_vk_mgr, g_seat);
    upload();
    return true;
  }
  void send_char(uint32_t cp, unsigned mods) override {
    uint32_t sym = xkb_utf32_to_keysym(cp);
    if (cp == ' ') sym = XKB_KEY_space;
    tap(sym, mods);
  }
  void send_special(fld::OskSpecial key, unsigned mods) override {
    uint32_t sym = 0;
    switch (key) {
      case fld::OSK_BACKSPACE: sym = XKB_KEY_BackSpace; break;
      case fld::OSK_ENTER: sym = XKB_KEY_Return; break;
      case fld::OSK_TAB: sym = XKB_KEY_Tab; break;
      case fld::OSK_ESCAPE: sym = XKB_KEY_Escape; break;
      case fld::OSK_LEFT: sym = XKB_KEY_Left; break;
      case fld::OSK_RIGHT: sym = XKB_KEY_Right; break;
      case fld::OSK_UP: sym = XKB_KEY_Up; break;
      case fld::OSK_DOWN: sym = XKB_KEY_Down; break;
      case fld::OSK_DELETE: sym = XKB_KEY_Delete; break;
      case fld::OSK_HOME: sym = XKB_KEY_Home; break;
      case fld::OSK_END: sym = XKB_KEY_End; break;
      default: return;
    }
    tap(sym, mods);
  }

private:
  zwp_virtual_keyboard_v1 *vk_ = nullptr;
  VKeymap map_;

  static uint32_t now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
  }

  void upload() {
    std::string km = map_.text();
    int fd = memfd_create("fl-osk-keymap", MFD_CLOEXEC);
    if (fd < 0) return;
    if (write(fd, km.c_str(), km.size() + 1) != (ssize_t)km.size() + 1) { close(fd); return; }
    zwp_virtual_keyboard_v1_keymap(vk_, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)km.size() + 1);
    close(fd);
  }

  void tap(uint32_t sym, unsigned mods) {
    if (!vk_ || !sym) return;
    bool changed;
    uint32_t code = map_.keycode(sym, changed);
    if (changed) upload();
    uint32_t m = 0;
    if (mods & fld::OSK_MOD_SHIFT) m |= VKeymap::SHIFT;
    if (mods & fld::OSK_MOD_CTRL) m |= VKeymap::CTRL;
    if (mods & fld::OSK_MOD_ALT) m |= VKeymap::ALT;
    // Shift is only sent for special keys (Shift+Tab, selection); characters
    // are sent as the final keysym
    if (m) zwp_virtual_keyboard_v1_modifiers(vk_, m, 0, 0, 0);
    zwp_virtual_keyboard_v1_key(vk_, now_ms(), code, WL_KEYBOARD_KEY_STATE_PRESSED);
    zwp_virtual_keyboard_v1_key(vk_, now_ms(), code, WL_KEYBOARD_KEY_STATE_RELEASED);
    if (m) zwp_virtual_keyboard_v1_modifiers(vk_, 0, 0, 0, 0);
    wl_display_flush(fl_wl_display());
  }
};

static std::string pidfile() {
  return fld::getenv_or("XDG_RUNTIME_DIR", "/tmp") + "/fl-osk-" + std::to_string(getuid()) + ".pid";
}

static bool toggle_existing() {
  std::string s;
  if (!fld::read_file(pidfile(), s)) return false;
  pid_t pid = atoi(s.c_str());
  if (pid <= 1 || pid == getpid()) return false;
  std::string comm;
  fld::read_file("/proc/" + std::to_string(pid) + "/comm", comm);
  if (fld::trim(comm) != "fl-osk") return false;
  return kill(pid, SIGTERM) == 0;
}

static void on_term(int) {
  unlink(pidfile().c_str());
  _exit(0);
}

static std::string config_path() { return fld::fld_config_dir() + "/osk.ini"; }

int main(int argc, char **argv) {
  std::string layout;
  bool toggle = true;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--layout" && i + 1 < argc) layout = argv[++i];
    else if (a == "--no-toggle") toggle = false;
    else if (a == "-h" || a == "--help") {
      printf("Usage: fl-osk [--layout us|de|fr] [--no-toggle]\nStarting it while it runs closes it.\n");
      return 0;
    }
  }
  if (toggle && toggle_existing()) return 0;
  fld::write_file(pidfile(), std::to_string(getpid()));
  signal(SIGTERM, on_term);

  fld::init_app("fldesktop.osk", 0, nullptr);
  // The shell window is borderless and fl-wm knows it by its title; any other
  // window (dialogs, fl_choice) gets an ordinary app id, or fl-wm would give
  // it the shell role too.
  Fl_Window::default_xclass("fl-osk");
  fld::Ini cfg;
  cfg.load(config_path());
  if (layout.empty()) layout = cfg.get("Keyboard", "Layout", "us");
  int height = cfg.get_int("Keyboard", "Height", 0);
  int W = Fl::w() > 0 ? Fl::w() : 1280;
  int maxw = cfg.get_int("Keyboard", "MaxWidth", 1200);
  if (W > maxw) W = maxw;
  if (height <= 0) height = std::max(200, std::min(320, (Fl::h() > 0 ? Fl::h() : 720) * 34 / 100));

  Fl_Double_Window *win = new Fl_Double_Window(W, height, "fldesktop.osk");
  win->border(0);
  fld::OskWidget *osk = new fld::OskWidget(0, 0, W, height);
  osk->layout(layout);
  osk->on_hide = [](fld::OskWidget *, void *) { on_term(0); };
  osk->on_layout_changed = [](fld::OskWidget *o, void *) {
    fld::Ini c;
    c.load(config_path());
    c.set("Keyboard", "Layout", o->layout_id());
    c.save(config_path());
  };
  win->resizable(osk);
  win->end();
  win->show();
  VirtualKeyboardSink sink;
  if (!sink.init()) {
    fprintf(stderr, "fl-osk: the compositor does not support the virtual keyboard protocol\n");
    unlink(pidfile().c_str());
    return 1;
  }
  osk->sink(&sink);
  int r = Fl::run();
  unlink(pidfile().c_str());
  return r;
}
