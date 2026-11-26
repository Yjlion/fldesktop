// fl-settings: search across the pages
#include "settings_search.h"
#include "test.h"

using namespace settings_search;

int main() {
  std::vector<Item> items = {{"mouse", "Natural scrolling (content follows the fingers)"},
                             {"mouse", "Tap to click"},
                             {"displays", "Night Light"},
                             {"displays", "Warmer colours at night"},
                             {"power", "Show the battery percentage in the panel"},
                             {"panel", "&Clock format:"}};
  auto h = search("night", items);
  CHECK_EQ(h.size(), (size_t)2);
  CHECK_EQ(h[0].label, std::string("Night Light")); // starts with it
  CHECK_EQ(h[0].page, std::string("displays"));
  h = search("tap click", items);
  CHECK_EQ(h.size(), (size_t)1);
  CHECK_EQ(h[0].page, std::string("mouse"));
  CHECK(search("scroll", items).size() == 1);
  CHECK(search("percent", items).size() == 1);   // a word's start
  CHECK(search("oll", items).empty());            // short pieces must start a word
  CHECK(search("ollin", items).size() == 1);      // longer ones may be inside
  CHECK(search("follows", items).size() == 1);
  CHECK(search("clock", items).size() == 1);      // without the & marker
  CHECK(search("", items).empty());
  CHECK(search("  ", items).empty());
  CHECK(search("night zebra", items).empty());
  CHECK_EQ(normalize("&Clock format:"), std::string("clock format"));
  CHECK_EQ(normalize("@-> Next…"), std::string("next"));
  CHECK_EQ(normalize("Mouse & Touchpad"), std::string("mouse & touchpad"));
  CHECK(search("a", items, 1).size() <= 1);
  // the generated table is there and names pages
  CHECK(index().size() > 100);
  bool night = false;
  for (auto &it : index()) night = night || std::string(it.label) == "Warmer colours at night";
  CHECK(night);
  return test_result("settings-search");
}
