#include "vpn_util.h"
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  std::string s(reinterpret_cast<const char *>(data), size), err;
  nm::WgConfig c;
  nm::parse_wg_quick(s, c, err);
  nm::format_kv(nm::parse_kv(s));
  nm::wg_iface_name(s);
  nm::valid_wg_key(s);
  return 0;
}
