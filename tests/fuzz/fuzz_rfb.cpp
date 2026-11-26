// The VNC client state machine, fed what a (hostile) server sends. The input
// is split at a 0xff 0x00 marker so later chunks arrive as separate reads.
#include "rfb.h"
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  rfb::Client c("password");
  c.on_need_password = [&] { c.set_password("pw"); c.password_given(); };
  size_t start = 0;
  for (size_t i = 0; i + 1 < size; i++)
    if (data[i] == 0xff && data[i + 1] == 0x00) {
      if (!c.feed(reinterpret_cast<const char *>(data) + start, i - start)) return 0;
      c.take_output();
      start = i + 2;
    }
  if (start < size) c.feed(reinterpret_cast<const char *>(data) + start, size - start);
  c.take_output();
  if (c.state() == rfb::Client::NORMAL) {
    c.pointer(1, 1, 1);
    c.key('a', true);
    c.request_update(true);
  }
  return 0;
}
