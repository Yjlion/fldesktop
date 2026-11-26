#include "playlist_util.h"
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  std::string s(reinterpret_cast<const char *>(data), size);
  auto m3u = media::parse_playlist(s, "/music", false);
  auto pls = media::parse_playlist(s, "/music", true);
  media::parse_playlist(media::write_m3u(m3u), "/music", false);
  media::parse_playlist(media::write_pls(pls), "/music", true);
  return 0;
}
