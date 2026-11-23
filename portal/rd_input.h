// RemoteDesktop input: a virtual pointer (wlr-virtual-pointer-unstable-v1)
// and a virtual keyboard (virtual-keyboard-unstable-v1) on a Wayland
// connection of our own. Call from one thread (the GLib thread).
#pragma once
#include <cstdint>
#include <string>

namespace portal::rd {

// Connects and creates the devices; false (and err) without the protocols.
bool open(std::string &err);
void close();
bool is_open();

void motion(double dx, double dy);
// x, y in the logical coordinates of the output called `output`
void motion_absolute(const std::string &output, double x, double y);
void button(int evdev_button, bool pressed);
void axis(double dx, double dy, bool finish);
void axis_discrete(int axis, int steps); // axis 0 vertical, 1 horizontal
void key(int evdev_keycode, bool pressed);
// a keysym through the keyboard's layout; false when no key makes it
bool keysym(uint32_t sym, bool pressed);

} // namespace portal::rd
