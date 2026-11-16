// Builds xkb keymaps for the virtual keyboard: every keysym the keyboard sends
// gets its own keycode, so any Unicode character can be typed (like wtype).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

class VKeymap {
public:
  VKeymap();
  // Returns the Wayland (evdev) keycode for a keysym; adds it if needed and sets
  // `changed` so the caller re-uploads the keymap.
  uint32_t keycode(uint32_t keysym, bool &changed);
  std::string text() const;
  size_t size() const { return syms_.size(); }
  // modifier masks for the "complete" xkb types
  static const uint32_t SHIFT = 1, CTRL = 4, ALT = 8;
private:
  std::vector<uint32_t> syms_;
};
