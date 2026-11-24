#include "spell_util.h"

#include <cctype>

namespace spell {

namespace {
bool is_letter(unsigned char c) { return isalpha(c) || c >= 0x80; }
bool is_space(unsigned char c) { return isspace(c) || c == '"' || c == '(' || c == ')' || c == '<' || c == '>'; }

// a URL, e-mail address or path: the whole space-separated token is skipped
bool skip_token(const std::string &t, size_t a, size_t b) {
  std::string tok = t.substr(a, b - a);
  return tok.find("://") != std::string::npos || tok.compare(0, 4, "www.") == 0 ||
         (tok.find('@') != std::string::npos && tok.find('.') != std::string::npos) ||
         tok.find('/') != std::string::npos || tok.find('\\') != std::string::npos ||
         tok.find("::") != std::string::npos || tok.find("->") != std::string::npos;
}
} // namespace

std::vector<Word> words(const std::string &text, size_t from, size_t to) {
  std::vector<Word> out;
  if (to > text.size()) to = text.size();
  size_t i = from;
  while (i < to) {
    // one space-separated token at a time
    while (i < to && is_space((unsigned char)text[i])) i++;
    size_t ta = i;
    while (i < to && !is_space((unsigned char)text[i])) i++;
    size_t tb = i;
    if (ta == tb || skip_token(text, ta, tb)) continue;
    size_t j = ta;
    while (j < tb) {
      while (j < tb && !is_letter((unsigned char)text[j]) && !isdigit((unsigned char)text[j]) && text[j] != '_') j++;
      size_t wa = j;
      // camelCase and ALLCAPS (an upper-case letter after the first) are names
      bool digit = false, under = false, upper_inside = false;
      while (j < tb) {
        unsigned char c = (unsigned char)text[j];
        bool apostrophe = (c == '\'' || c == '-') && j > wa && j + 1 < tb && is_letter((unsigned char)text[j + 1]);
        if (!(is_letter(c) || isdigit(c) || c == '_' || apostrophe)) break;
        if (c == '-') break; // well-known: each part of a hyphenated word on its own
        if (isdigit(c)) digit = true;
        if (c == '_') under = true;
        if (isupper(c) && j > wa) upper_inside = true;
        j++;
      }
      size_t len = j - wa;
      size_t chars = 0; // UTF-8 characters, not bytes
      for (size_t k = wa; k < j; k++) chars += ((unsigned char)text[k] & 0xc0) != 0x80;
      if (len && chars > 1 && !digit && !under && !upper_inside) out.push_back({wa, len});
      if (j < tb && text[j] == '-') j++;
    }
  }
  return out;
}

std::string language_from_locale(const std::string &locale) {
  std::string l = locale.substr(0, locale.find_first_of(".@"));
  if (l.empty() || l == "C" || l == "POSIX") return "en_US";
  return l;
}

std::vector<std::string> split_languages(const std::string &list) {
  std::vector<std::string> v;
  std::string cur;
  for (char c : list + ",") {
    if (c == ',' || c == ';' || c == ' ') {
      if (!cur.empty()) v.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  return v;
}

} // namespace spell
