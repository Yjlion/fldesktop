// Unicode data for fl-charmap: blocks (Blocks.txt) and character names
// (UnicodeData.txt). No FLTK dependency so it can be unit tested.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace charmap {

struct Block {
  uint32_t first, last;
  std::string name;
};

struct CharInfo {
  uint32_t cp;
  std::string name;     // e.g. "LATIN SMALL LETTER A"
  char category[3];     // general category, e.g. "Ll"
};

std::vector<Block> parse_blocks(const std::string &text);

class NameDb {
public:
  bool load(const std::string &unicode_data_path);
  bool parse(const std::string &text);
  bool empty() const { return chars_.empty() && ranges_.empty(); }
  // Name of a code point ("" if unknown). Ideograph and other range entries
  // get generated names, e.g. "CJK UNIFIED IDEOGRAPH-4E00".
  std::string name(uint32_t cp) const;
  std::string category(uint32_t cp) const;
  bool assigned(uint32_t cp) const;
  // printable: assigned and not a control, surrogate, private use or format char
  bool printable(uint32_t cp) const;
  // Code points whose name contains all words of the query (case-insensitive),
  // or the code point given as U+XXXX / 0xXXXX / a single character.
  std::vector<uint32_t> search(const std::string &query, size_t limit = 500) const;

private:
  std::vector<CharInfo> chars_; // sorted by code point
  struct Range { uint32_t first, last; std::string base; char category[3]; };
  std::vector<Range> ranges_;
  const CharInfo *find(uint32_t cp) const;
  const Range *find_range(uint32_t cp) const;
};

// Returns the first existing UnicodeData.txt from the usual places, or "".
std::string find_unicode_data();
std::string find_blocks(const std::string &fallback_dir);
// UTF-8 encoding of a code point
std::string utf8(uint32_t cp);
// Decodes the first character of a UTF-8 string (0 if empty)
uint32_t first_codepoint(const std::string &s);

} // namespace charmap
