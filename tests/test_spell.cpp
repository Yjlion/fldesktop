// Spell checking: which words get checked
#include <string>
#include <vector>

#include "spell_util.h"
#include "test.h"

static std::vector<std::string> W(const std::string &t) {
  std::vector<std::string> v;
  for (auto &w : spell::words(t)) v.push_back(t.substr(w.start, w.len));
  return v;
}

int main() {
  using V = std::vector<std::string>;
  CHECK(W("Hello wrold, this is a tst.") == V({"Hello", "wrold", "this", "is", "tst"}));
  // names and code: camelCase, ALLCAPS, snake_case, digits
  CHECK(W("call getValue and MAX_SIZE or HTTP and x86 now") == V({"call", "and", "or", "and", "now"}));
  // URLs, mail addresses and paths are skipped whole
  CHECK(W("see https://example.org/wrng and me@exmple.org or /usr/lib/foo") == V({"see", "and", "or"}));
  // apostrophes inside words, hyphenated parts on their own
  CHECK(W("don't well-known 'quoted'") == V({"don't", "well", "known", "quoted"}));
  // UTF-8 letters are part of words
  CHECK(W("Größe café") == V({"Größe", "café"}));
  // a range of the text
  std::string t = "// a comment wrod\nint x;";
  auto w = spell::words(t, 3, t.find('\n'));
  CHECK_EQ(w.size(), (size_t)2);
  CHECK_EQ(spell::language_from_locale("de_DE.UTF-8"), std::string("de_DE"));
  CHECK_EQ(spell::language_from_locale("C.UTF-8"), std::string("en_US"));
  CHECK(spell::split_languages("en_US, de_DE") == V({"en_US", "de_DE"}));
  return test_result("spell");
}
