// The generated virtual keyboard keymap must compile and map keycodes back to the keysyms
#include <xkbcommon/xkbcommon.h>

#include "test.h"
#include "vkeymap.h"

int main() {
  VKeymap m;
  bool changed;
  uint32_t a = m.keycode(XKB_KEY_a, changed);
  CHECK(changed);
  uint32_t a2 = m.keycode(XKB_KEY_a, changed);
  CHECK(!changed);
  CHECK_EQ(a, a2);
  uint32_t euro = m.keycode(xkb_utf32_to_keysym(0x20AC), changed);
  uint32_t han = m.keycode(xkb_utf32_to_keysym(0x6F22), changed); // a CJK character
  uint32_t bs = m.keycode(XKB_KEY_BackSpace, changed);
  xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  std::string text = m.text();
  xkb_keymap *km = xkb_keymap_new_from_string(ctx, text.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  CHECK(km != nullptr);
  if (km) {
    xkb_state *st = xkb_state_new(km);
    CHECK_EQ(xkb_state_key_get_one_sym(st, a + 8), (xkb_keysym_t)XKB_KEY_a);
    CHECK_EQ(xkb_state_key_get_utf32(st, euro + 8), 0x20ACu);
    CHECK_EQ(xkb_state_key_get_utf32(st, han + 8), 0x6F22u);
    CHECK_EQ(xkb_state_key_get_one_sym(st, bs + 8), (xkb_keysym_t)XKB_KEY_BackSpace);
    // Control is a real modifier usable by clients
    CHECK(xkb_keymap_mod_get_index(km, XKB_MOD_NAME_CTRL) != XKB_MOD_INVALID);
    xkb_state_update_mask(st, VKeymap::CTRL, 0, 0, 0, 0, 0);
    CHECK(xkb_state_mod_name_is_active(st, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0);
    xkb_state_unref(st);
    xkb_keymap_unref(km);
  }
  xkb_context_unref(ctx);
  return test_result("vkeymap");
}
