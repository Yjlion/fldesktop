// notification helpers: markup stripping and image-data conversion
#include <vector>

#include "fld/notify.h"
#include "test.h"

int main() {
  using fld::notify_strip_markup;
  CHECK_EQ(notify_strip_markup("plain"), std::string("plain"));
  CHECK_EQ(notify_strip_markup("<b>bold</b> and <i>it</i>"), std::string("bold and it"));
  CHECK_EQ(notify_strip_markup("a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos;"), std::string("a & b <c> \"d\" 'e'"));
  CHECK_EQ(notify_strip_markup("<a href=\"https://x\">link</a>"), std::string("link"));
  CHECK_EQ(notify_strip_markup("one<br/>two"), std::string("one\ntwo"));
  CHECK_EQ(notify_strip_markup("&#233;&#x263A;"), std::string("é☺"));
  CHECK_EQ(notify_strip_markup("5 < 6 & ok"), std::string("5 < 6 & ok"));
  CHECK_EQ(notify_strip_markup("AT&T"), std::string("AT&T"));

  // 2x2 RGB with a padded row stride of 8 bytes
  std::vector<unsigned char> rgb = {1, 2, 3, 4, 5, 6, 0, 0, 7, 8, 9, 10, 11, 12};
  std::vector<unsigned char> out;
  CHECK(fld::notify_image_to_rgba(2, 2, 8, false, 8, 3, rgb.data(), rgb.size(), out));
  std::vector<unsigned char> want = {1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255};
  CHECK(out == want);
  // RGBA passes through
  std::vector<unsigned char> rgba = {1, 2, 3, 4};
  CHECK(fld::notify_image_to_rgba(1, 1, 4, true, 8, 4, rgba.data(), rgba.size(), out));
  CHECK(out == rgba);
  // too short, wrong channel count, wrong depth
  CHECK(!fld::notify_image_to_rgba(2, 2, 8, false, 8, 3, rgb.data(), 10, out));
  CHECK(!fld::notify_image_to_rgba(1, 1, 4, true, 8, 3, rgba.data(), 4, out));
  CHECK(!fld::notify_image_to_rgba(1, 1, 4, true, 16, 4, rgba.data(), 4, out));
  CHECK(!fld::notify_image_to_rgba(0, 1, 4, true, 8, 4, rgba.data(), 4, out));
  return test_result("notify");
}
