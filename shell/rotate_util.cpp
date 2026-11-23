#include "rotate_util.h"

namespace rotate {

int transform_for(const std::string &o) {
  // wl_output transforms turn counter-clockwise: the top of the picture
  // goes where the device's "up" now is
  if (o == "normal") return 0;    // WL_OUTPUT_TRANSFORM_NORMAL
  if (o == "left-up") return 1;   // _90
  if (o == "bottom-up") return 2; // _180
  if (o == "right-up") return 3;  // _270
  return -1;
}

bool is_internal(const std::string &n) {
  for (const char *p : {"eDP", "LVDS", "DSI"})
    if (n.compare(0, std::char_traits<char>::length(p), p) == 0) return true;
  return false;
}

} // namespace rotate
