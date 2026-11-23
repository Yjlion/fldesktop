// fl-autorotate: the accelerometer's orientation to an output transform
#pragma once
#include <string>

namespace rotate {
// iio-sensor-proxy's AccelerometerOrientation ("normal", "left-up",
// "right-up", "bottom-up") to a wl_output transform (0, 90, 180, 270 as
// WL_OUTPUT_TRANSFORM_NORMAL .. _270); -1 for "undefined" and the like.
int transform_for(const std::string &orientation);
// eDP-1, LVDS-1, DSI-1: the computer's own screen
bool is_internal(const std::string &output_name);
} // namespace rotate
