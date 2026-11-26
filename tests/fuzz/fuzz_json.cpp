#include <fld/json.h>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  fld::Json j;
  std::string err;
  if (fld::Json::parse(std::string(reinterpret_cast<const char *>(data), size), j, &err)) {
    fld::Json again;
    fld::Json::parse(j.dump(), again, &err);
  }
  return 0;
}
