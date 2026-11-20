// fl-cliphist: the history store and its helpers
#include <cstdlib>
#include <string>

#include "cliphist_store.h"
#include "test.h"

using namespace cliphist;

int main() {
  char tmpl[] = "/tmp/fl-cliphist-XXXXXX";
  std::string dir = std::string(mkdtemp(tmpl)) + "/clip";
  {
    Store s(dir);
    CHECK(s.list().empty());
    uint64_t a = s.add("text/plain;charset=utf-8", "first", 3);
    uint64_t b = s.add("text/plain;charset=utf-8", "second", 3);
    CHECK(a && b && a != b);
    CHECK_EQ(s.list().size(), (size_t)2);
    CHECK_EQ(s.list()[0].preview, std::string("second"));
    // the same text again moves to the top, no new entry
    CHECK_EQ(s.add("text/plain;charset=utf-8", "first", 3), a);
    CHECK_EQ(s.list().size(), (size_t)2);
    CHECK_EQ(s.list()[0].id, a);
    // pinned entries survive the cap and clearing
    CHECK(s.set_pinned(b, true));
    s.add("text/plain;charset=utf-8", "three", 3);
    s.add("text/plain;charset=utf-8", "four", 3);
    s.add("text/plain;charset=utf-8", "five", 3); // "first" falls out
    auto l = s.list();
    CHECK_EQ(l.size(), (size_t)4);
    bool first_gone = true, second_kept = false;
    for (auto &e : l) {
      if (e.preview == "first") first_gone = false;
      if (e.id == b && e.pinned) second_kept = true;
    }
    CHECK(first_gone);
    CHECK(second_kept);
    std::string mime, data;
    CHECK(s.data(b, mime, data));
    CHECK_EQ(data, std::string("second"));
    CHECK(!s.data(a, mime, data));
    CHECK(s.remove(l[0].id));
    CHECK(!s.remove(l[0].id));
    s.clear(true);
    CHECK_EQ(s.list().size(), (size_t)1);
    CHECK_EQ(s.list()[0].id, b);
    // a second Store on the same directory sees the same history
    CHECK_EQ(Store(dir).list().size(), (size_t)1);
  }
  // previews
  CHECK_EQ(text_preview("  a\n\tb  c \n"), std::string("a b c"));
  CHECK_EQ(text_preview("abcdef", 3), std::string("abc…"));
  CHECK_EQ(text_preview("äöü", 2), std::string("äö…")); // whole UTF-8 characters
  // PNG size
  std::string png("\x89PNG\r\n\x1a\n\0\0\0\x0dIHDR\0\0\x01\x40\0\0\0\xf0", 24);
  int w, h;
  CHECK(png_size(png, &w, &h));
  CHECK_EQ(w, 320);
  CHECK_EQ(h, 240);
  CHECK(!png_size("not a png at all, really", &w, &h));
  {
    Store s(dir);
    s.add("image/png", png, 10);
    CHECK_EQ(s.list()[0].preview, std::string("Image 320×240"));
    CHECK(s.list()[0].is_image());
  }
  // what to keep
  CHECK_EQ(pick_mime({"text/html", "text/plain", "text/plain;charset=utf-8"}), std::string("text/plain;charset=utf-8"));
  CHECK_EQ(pick_mime({"UTF8_STRING", "STRING"}), std::string("UTF8_STRING"));
  CHECK_EQ(pick_mime({"image/jpeg", "image/png"}), std::string("image/png"));
  CHECK_EQ(pick_mime({"text/plain", "x-kde-passwordManagerHint"}), std::string(""));
  CHECK_EQ(pick_mime({"application/x-whatever"}), std::string(""));
  system(("rm -rf " + std::string(tmpl)).c_str());
  return test_result("cliphist");
}
