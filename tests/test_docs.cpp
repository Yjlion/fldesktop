// fl-docs: layout, zoom, coordinates and print ranges
#include "docs_util.h"
#include "test.h"

using namespace docs;

int main() {
  std::vector<PageSize> pages = {{612, 792}, {612, 792}, {792, 612}}; // letter, letter, landscape
  Layout l = layout(pages, 1.0);
  CHECK_EQ(l.w[0], 816); // 8.5 in at 96 dpi
  CHECK_EQ(l.h[0], 1056);
  CHECK_EQ(l.y[0], kGap);
  CHECK_EQ(l.y[1], kGap + 1056 + kGap);
  CHECK_EQ(l.width, 1056 + 2 * kGap); // the landscape page is the widest
  CHECK_EQ(l.x[0], (l.width - 816) / 2);
  CHECK_EQ(l.height, kGap + 1056 + kGap + 1056 + kGap + 816 + kGap);
  Layout wide = layout(pages, 1.0, 2000);
  CHECK_EQ(wide.width, 2000);
  CHECK_EQ(wide.x[2], (2000 - 1056) / 2);

  CHECK_EQ(page_at(l, 0), 0);
  CHECK_EQ(page_at(l, 500), 0);
  CHECK_EQ(page_at(l, l.y[1] + 10), 1);
  CHECK_EQ(page_at(l, 1000000), 2);
  CHECK_EQ(page_at(Layout{}, 10), -1);
  CHECK_EQ(current_page(l, l.y[1] - 100, 800), 1); // most of the view shows page 2
  CHECK_EQ(current_page(l, 0, 600), 0);

  double zw = fit_zoom(pages, 0, Fit::Width, 840, 600);
  CHECK(std::fabs(zw - 816.0 / 1056.0) < 0.01); // the landscape page is the widest
  CHECK(std::fabs(fit_zoom({{612, 792}}, 0, Fit::Width, 840, 600) - 1.0) < 0.01);
  double zp = fit_zoom(pages, 0, Fit::Page, 840, 600);
  CHECK(zp < zw);
  CHECK_EQ(fit_zoom(pages, 0, Fit::None, 840, 600), 1.0);
  CHECK_EQ(zoom_step(1.0, 1), 1.1);
  CHECK_EQ(zoom_step(1.0, -1), 0.9);
  CHECK_EQ(zoom_step(1.05, 1), 1.1);
  CHECK_EQ(zoom_step(8.0, 1), 8.0);
  CHECK_EQ(zoom_step(0.1, -1), 0.1);

  Rect r = to_strip(l, 1, 72, 72, 72, 18); // one inch in, one inch wide
  CHECK_EQ(r.x, l.x[1] + 96);
  CHECK_EQ(r.y, l.y[1] + 96);
  CHECK_EQ(r.w, 96);
  CHECK_EQ(r.h, 24);
  CHECK_EQ(to_strip(l, 9, 0, 0, 1, 1).w, 0);

  bool ok;
  CHECK_EQ(page_ranges("", 10, &ok), std::string(""));
  CHECK(ok);
  CHECK_EQ(page_ranges("All", 10, &ok), std::string(""));
  CHECK_EQ(page_ranges("1-3, 5, 8-", 10, &ok), std::string("1-3,5,8-10"));
  CHECK(ok);
  CHECK_EQ(page_ranges("-2,4", 10, &ok), std::string("1-2,4"));
  CHECK_EQ(page_ranges("7-20", 10, &ok), std::string("7-10"));
  page_ranges("11", 10, &ok);
  CHECK(!ok);
  page_ranges("3-1", 10, &ok);
  CHECK(!ok);
  page_ranges("a-b", 10, &ok);
  CHECK(!ok);
  page_ranges(",", 10, &ok);
  CHECK(!ok);

  // file formats
  CHECK(sniff("%PDF-1.7\n", "x") == Format::Pdf);
  CHECK(sniff("junk\n%PDF-1.4", "x") == Format::Pdf);
  CHECK(sniff("hello", "a.pdf") == Format::Pdf);
  CHECK(sniff("hello", "a.txt") == Format::Unknown);
  auto zip_with_mimetype = [](const std::string &mime) {
    std::string h = "PK\x03\x04";
    h += std::string(4, '\0');                   // version, flags
    h += std::string(2, '\0');                   // stored
    h += std::string(8, '\0');                   // time, date, crc
    for (int k = 0; k < 2; k++)                  // compressed and real size
      for (int b = 0; b < 4; b++) h += (char)((mime.size() >> (8 * b)) & 0xff);
    h += std::string("\x08\0\0\0", 4);          // name length 8, no extra
    return h + "mimetype" + mime + "PK\x03\x04";
  };
  CHECK(sniff(zip_with_mimetype("application/epub+zip"), "x") == Format::Epub);
  CHECK(sniff(zip_with_mimetype("application/vnd.oasis.opendocument.text"), "x") == Format::Odt);
  CHECK(sniff(zip_with_mimetype("application/vnd.oasis.opendocument.spreadsheet"), "x") == Format::OdfOther);
  CHECK(sniff(std::string("PK\x03\x04", 4) + std::string(60, 'x'), "x.cbz") == Format::Zip);
  CHECK(sniff(std::string("Rar!\x1a\x07\x00", 7), "x.cbr") == Format::Archive);

  CHECK(natural_less("page2.png", "page10.png"));
  CHECK(!natural_less("page10.png", "page2.png"));
  CHECK(natural_less("a/Page1.png", "a/page2.png"));
  CHECK(natural_less("01.jpg", "2.jpg"));
  CHECK(natural_less("ch1/10.png", "ch2/1.png"));
  CHECK(!natural_less("x", "x"));

  int iw, ih;
  std::string png = std::string("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\x01\x2c\0\0\x01\x90", 24) + std::string(8, '\0');
  CHECK(image_size(png, iw, ih));
  CHECK_EQ(iw, 300);
  CHECK_EQ(ih, 400);
  std::string gif = std::string("GIF89a\x0a\0\x05\0", 10) + std::string(4, '\0');
  CHECK(image_size(gif, iw, ih));
  CHECK_EQ(iw, 10);
  CHECK_EQ(ih, 5);
  // JPEG: SOI, an APP0 segment, then SOF0 with 16 x 8
  std::string jpg = std::string("\xff\xd8\xff\xe0\x00\x04\x00\x00\xff\xc0\x00\x11\x08\x00\x08\x00\x10\x03", 18) +
                    std::string(16, '\0');
  CHECK(image_size(jpg, iw, ih));
  CHECK_EQ(iw, 16);
  CHECK_EQ(ih, 8);
  CHECK(!image_size("not an image", iw, ih));
  CHECK(is_image_name("a/B.JPG"));
  CHECK(!is_image_name("a/b.txt"));
  CHECK(!is_image_name("png"));

  CHECK_NEAR(parse_length("1in", 0), 72.0);
  CHECK_NEAR(parse_length("2.54cm", 0), 72.0);
  CHECK(std::fabs(parse_length("10mm", 0) - 28.3465) < 1e-3);
  CHECK_NEAR(parse_length("12pt", 0), 12.0);
  CHECK_NEAR(parse_length("16px", 0), 12.0);
  CHECK_NEAR(parse_length("1pc", 0), 12.0);
  CHECK_NEAR(parse_length("bogus", 5), 5.0);
  CHECK_NEAR(parse_length("3em", 5), 5.0);
  return test_result("docs");
}
