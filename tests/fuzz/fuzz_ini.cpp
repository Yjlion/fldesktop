#include <fld/ini.h>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fld::Ini ini;
  ini.parse(std::string(reinterpret_cast<const char *>(data), size));
  for (auto &g : ini.groups())
    for (auto &k : ini.keys(g)) {
      ini.get(g, k);
      ini.get_int(g, k);
      ini.get_double(g, k);
      ini.get_bool(g, k);
    }
  // a round trip parses again
  fld::Ini again;
  again.parse(ini.to_string());
  return 0;
}
