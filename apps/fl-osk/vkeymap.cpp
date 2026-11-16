#include "vkeymap.h"

#include <cstdio>
#include <xkbcommon/xkbcommon.h>

VKeymap::VKeymap() {
  // modifier keys first so their keycodes are stable
  bool c;
  keycode(XKB_KEY_Shift_L, c);
  keycode(XKB_KEY_Control_L, c);
  keycode(XKB_KEY_Alt_L, c);
}

uint32_t VKeymap::keycode(uint32_t keysym, bool &changed) {
  changed = false;
  for (size_t i = 0; i < syms_.size(); i++)
    if (syms_[i] == keysym) return (uint32_t)i + 1;
  syms_.push_back(keysym);
  changed = true;
  return (uint32_t)syms_.size();
}

std::string VKeymap::text() const {
  // xkb keycodes are the Wayland keycodes + 8
  std::string s = "xkb_keymap {\nxkb_keycodes \"fl-osk\" {\n minimum = 8;\n";
  char buf[128];
  snprintf(buf, sizeof buf, " maximum = %zu;\n", syms_.size() + 8 + 1);
  s += buf;
  for (size_t i = 0; i < syms_.size(); i++) {
    snprintf(buf, sizeof buf, " <K%zu> = %zu;\n", i + 1, i + 1 + 8);
    s += buf;
  }
  s += "};\nxkb_types \"fl-osk\" { include \"complete\" };\n"
       "xkb_compatibility \"fl-osk\" { include \"complete\" };\n"
       "xkb_symbols \"fl-osk\" {\n";
  for (size_t i = 0; i < syms_.size(); i++) {
    char name[64];
    if (xkb_keysym_get_name(syms_[i], name, sizeof name) < 0) snprintf(name, sizeof name, "0x%x", syms_[i]);
    snprintf(buf, sizeof buf, " key <K%zu> {[ %s ]};\n", i + 1, name);
    s += buf;
  }
  s += " modifier_map Shift { <K1> };\n modifier_map Control { <K2> };\n modifier_map Mod1 { <K3> };\n";
  s += "};\n};\n";
  return s;
}
