// fl-paint: OpenRaster zip and stack.xml
#include <cstring>
#include <zlib.h>
#include "ora.h"
#include "test.h"

int main() {
  std::vector<ora::ZipEntry> in = {{"mimetype", "image/openraster"}, {"stack.xml", "<x/>"}, {"data/a.png", std::string("\x89PNG\0\1", 6)}};
  std::string z = ora::zip_write(in);
  std::vector<ora::ZipEntry> out;
  CHECK(ora::zip_read(z, out));
  CHECK_EQ(out.size(), (size_t)3);
  CHECK_EQ(out[0].name, std::string("mimetype"));
  CHECK_EQ(out[2].data, in[2].data);
  // mimetype comes first, uncompressed, at offset 30 (as the format asks)
  CHECK_EQ(z.substr(30, 8), std::string("mimetype"));
  CHECK_EQ(z.substr(38, 16), std::string("image/openraster"));
  CHECK(!ora::zip_read("not a zip at all, really not", out));
  CHECK(!ora::zip_read(z.substr(0, z.size() / 2), out));
  CHECK_EQ(ora::crc32("123456789"), 0xCBF43926u);

  // a deflated entry (as Krita writes them)
  std::string plain(1000, 'a');
  std::string comp(2000, '\0');
  z_stream s;
  memset(&s, 0, sizeof s);
  deflateInit2(&s, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
  s.next_in = (Bytef *)plain.data();
  s.avail_in = (uInt)plain.size();
  s.next_out = (Bytef *)&comp[0];
  s.avail_out = (uInt)comp.size();
  deflate(&s, Z_FINISH);
  comp.resize(s.total_out);
  deflateEnd(&s);
  std::string d = ora::zip_write({{"stack.xml", comp}});
  // turn the stored entry into a deflated one: method 8, sizes, crc of the plain data
  auto put32 = [&](size_t p, uint32_t v) { for (int i = 0; i < 4; i++) d[p + i] = (char)((v >> (8 * i)) & 255); };
  d[8] = 8;
  put32(14, ora::crc32(plain));
  put32(22, (uint32_t)plain.size());
  size_t cd = d.find(std::string("PK\1\2", 4));
  d[cd + 10] = 8;
  put32(cd + 16, ora::crc32(plain));
  put32(cd + 24, (uint32_t)plain.size());
  CHECK(ora::zip_read(d, out));
  CHECK_EQ(out.size(), (size_t)1);
  if (!out.empty()) CHECK_EQ(out[0].data, plain);

  std::vector<ora::LayerInfo> layers = {{"Top \"one\" & <b>", "data/layer1.png", 3, 4, 0.5, false},
                                        {"Background", "data/layer0.png", 0, 0, 1.0, true}};
  std::string x = ora::stack_xml(640, 480, layers);
  int w = 0, h = 0;
  std::vector<ora::LayerInfo> back;
  CHECK(ora::parse_stack(x, w, h, back));
  CHECK_EQ(w, 640);
  CHECK_EQ(h, 480);
  CHECK_EQ(back.size(), (size_t)2);
  CHECK_EQ(back[0].name, std::string("Top \"one\" & <b>"));
  CHECK_EQ(back[0].x, 3);
  CHECK(!back[0].visible);
  CHECK(back[0].opacity > 0.49 && back[0].opacity < 0.51);
  CHECK(back[1].visible);
  CHECK(!ora::parse_stack("<image w='0' h='5'>", w, h, back));
  CHECK(ora::parse_stack("<image w='2' h='3'><stack><layer src='a.png' name='x'/></stack></image>", w, h, back));
  CHECK_EQ(back.size(), (size_t)1);
  return test_result("ora");
}
