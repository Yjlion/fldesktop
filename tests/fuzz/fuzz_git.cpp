#include <fld/git_status.h>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  std::string s(reinterpret_cast<const char *>(data), size);
  auto st = fld::git::parse_status(s);
  for (auto &kv : st) {
    fld::git::state_of(st, kv.first, false);
    fld::git::state_of(st, kv.first, true);
  }
  fld::git::state_of(st, "", true);
  fld::git::parse_diff(s);
  return 0;
}
