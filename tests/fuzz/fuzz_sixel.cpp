#include "sixel.h"
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  sixel::Image im;
  sixel::decode("0;1;0", std::string(reinterpret_cast<const char *>(data), size), im, 512, 512);
  return 0;
}
