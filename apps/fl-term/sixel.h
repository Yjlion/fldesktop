// Sixel images (DCS P1;P2;P3 q ... ST), decoded to RGBA. Pure code (unit
// tested, fuzzed); fl-term places the image at the cursor.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace sixel {

struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;  // w * h * 4; pixels no sixel set are transparent
};

// params: the DCS parameters before the 'q' ("0;1;0"); data: what follows
// it up to the string terminator. Images larger than max_w x max_h are cut
// there. False when nothing was drawn.
bool decode(const std::string &params, const std::string &data, Image &out, int max_w = 4096, int max_h = 4096);

}  // namespace sixel
