#include "charmap_data.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace charmap {

static std::string trim(const std::string &s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

static std::string upper(std::string s) {
  for (auto &c : s) c = (char)toupper((unsigned char)c);
  return s;
}

std::vector<Block> parse_blocks(const std::string &text) {
  // 0000..007F; Basic Latin
  std::vector<Block> v;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    size_t dots = line.find(".."), semi = line.find(';');
    if (dots == std::string::npos || semi == std::string::npos || dots > semi) continue;
    Block b;
    b.first = (uint32_t)strtoul(line.substr(0, dots).c_str(), nullptr, 16);
    b.last = (uint32_t)strtoul(line.substr(dots + 2, semi - dots - 2).c_str(), nullptr, 16);
    b.name = trim(line.substr(semi + 1));
    if (b.last >= b.first) v.push_back(b);
  }
  return v;
}

bool NameDb::load(const std::string &path) {
  std::ifstream f(path);
  if (!f) return false;
  std::stringstream ss;
  ss << f.rdbuf();
  return parse(ss.str());
}

bool NameDb::parse(const std::string &text) {
  chars_.clear();
  ranges_.clear();
  size_t pos = 0;
  Range pending{};
  bool have_pending = false;
  while (pos < text.size()) {
    size_t eol = text.find('\n', pos);
    if (eol == std::string::npos) eol = text.size();
    // fields: code;name;category;...;unicode1 name(10)
    const char *p = text.c_str() + pos;
    const char *s1 = (const char *)memchr(p, ';', eol - pos);
    if (s1) {
      const char *s2 = (const char *)memchr(s1 + 1, ';', text.c_str() + eol - s1 - 1);
      if (s2) {
        const char *s3 = (const char *)memchr(s2 + 1, ';', text.c_str() + eol - s2 - 1);
        uint32_t cp = (uint32_t)strtoul(p, nullptr, 16);
        std::string name(s1 + 1, s2);
        std::string cat = s3 ? std::string(s2 + 1, s3) : std::string(s2 + 1, text.c_str() + eol);
        if (!name.empty() && name[0] == '<' && name.find(", First>") != std::string::npos) {
          pending.first = cp;
          pending.base = name.substr(1, name.find(',') - 1);
          snprintf(pending.category, 3, "%s", cat.c_str());
          have_pending = true;
        } else if (!name.empty() && name[0] == '<' && name.find(", Last>") != std::string::npos && have_pending) {
          pending.last = cp;
          ranges_.push_back(pending);
          have_pending = false;
        } else {
          CharInfo ci;
          ci.cp = cp;
          // control characters are named "<control>"; use the Unicode 1 name if present
          if (name == "<control>") {
            int field = 2;
            const char *q = s2;
            while (q && field < 10) {
              q = (const char *)memchr(q + 1, ';', text.c_str() + eol - q - 1);
              field++;
            }
            if (q) {
              const char *e = (const char *)memchr(q + 1, ';', text.c_str() + eol - q - 1);
              std::string u1 = e ? std::string(q + 1, e) : "";
              name = u1.empty() ? "<control>" : u1;
            }
          }
          ci.name = name;
          snprintf(ci.category, 3, "%s", cat.c_str());
          chars_.push_back(ci);
        }
      }
    }
    pos = eol + 1;
  }
  std::sort(chars_.begin(), chars_.end(), [](const CharInfo &a, const CharInfo &b) { return a.cp < b.cp; });
  return !chars_.empty();
}

const CharInfo *NameDb::find(uint32_t cp) const {
  auto it = std::lower_bound(chars_.begin(), chars_.end(), cp,
                             [](const CharInfo &c, uint32_t v) { return c.cp < v; });
  return (it != chars_.end() && it->cp == cp) ? &*it : nullptr;
}

const NameDb::Range *NameDb::find_range(uint32_t cp) const {
  for (auto &r : ranges_)
    if (cp >= r.first && cp <= r.last) return &r;
  return nullptr;
}

std::string NameDb::name(uint32_t cp) const {
  if (const CharInfo *c = find(cp)) return c->name;
  if (const Range *r = find_range(cp)) {
    char buf[96];
    std::string base = upper(r->base);
    if (base.find("IDEOGRAPH") != std::string::npos || base.find("TANGUT") != std::string::npos ||
        base.find("KHITAN") != std::string::npos || base.find("NUSHU") != std::string::npos)
      snprintf(buf, sizeof buf, "%s-%04X", base.c_str(), cp);
    else
      snprintf(buf, sizeof buf, "%s", base.c_str());
    // "CJK Ideograph Extension A" -> "CJK UNIFIED IDEOGRAPH-3400"
    std::string s = buf;
    if (s.rfind("CJK IDEOGRAPH", 0) == 0) {
      snprintf(buf, sizeof buf, "CJK UNIFIED IDEOGRAPH-%04X", cp);
      s = buf;
    }
    return s;
  }
  return "";
}

std::string NameDb::category(uint32_t cp) const {
  if (const CharInfo *c = find(cp)) return c->category;
  if (const Range *r = find_range(cp)) return r->category;
  return "Cn";
}

bool NameDb::assigned(uint32_t cp) const { return find(cp) || find_range(cp); }

bool NameDb::printable(uint32_t cp) const {
  std::string c = category(cp);
  return !(c == "Cn" || c == "Cc" || c == "Cs" || c == "Co" || c == "Cf" || c == "Zl" || c == "Zp");
}

std::vector<uint32_t> NameDb::search(const std::string &query_in, size_t limit) const {
  std::vector<uint32_t> out;
  std::string q = trim(query_in);
  if (q.empty()) return out;
  // U+XXXX, 0xXXXX or plain hex of 4+ digits
  std::string uq = upper(q);
  std::string hex;
  if (uq.rfind("U+", 0) == 0) hex = uq.substr(2);
  else if (uq.rfind("0X", 0) == 0) hex = uq.substr(2);
  else if (uq.size() >= 4 && uq.size() <= 6 && uq.find_first_not_of("0123456789ABCDEF") == std::string::npos) hex = uq;
  if (!hex.empty() && hex.find_first_not_of("0123456789ABCDEF") == std::string::npos) {
    uint32_t cp = (uint32_t)strtoul(hex.c_str(), nullptr, 16);
    if (cp <= 0x10FFFF) out.push_back(cp);
  }
  // a single (non-ASCII-letter) character
  uint32_t first = first_codepoint(q);
  if (utf8(first) == q && (first > 127 || !isalnum((int)first))) {
    if (std::find(out.begin(), out.end(), first) == out.end()) out.push_back(first);
    return out;
  }
  // all words in the name
  std::vector<std::string> words;
  std::istringstream ws(uq);
  std::string w;
  while (ws >> w) words.push_back(w);
  auto matches = [&](const std::string &name) {
    for (auto &x : words)
      if (name.find(x) == std::string::npos) return false;
    return true;
  };
  // exact name first
  for (auto &c : chars_)
    if (c.name == uq && std::find(out.begin(), out.end(), c.cp) == out.end()) out.push_back(c.cp);
  for (auto &c : chars_) {
    if (out.size() >= limit) break;
    if (matches(c.name) && std::find(out.begin(), out.end(), c.cp) == out.end()) out.push_back(c.cp);
  }
  return out;
}

static bool exists(const std::string &p) {
  struct stat st;
  return stat(p.c_str(), &st) == 0;
}

std::string find_unicode_data() {
  for (const char *p : {"/usr/share/unicode/UnicodeData.txt", "/usr/share/unicode-data/UnicodeData.txt",
                        "/usr/share/unicode/ucd/UnicodeData.txt"})
    if (exists(p)) return p;
  return "";
}

std::string find_blocks(const std::string &fallback_dir) {
  for (std::string p : {std::string("/usr/share/unicode/Blocks.txt"), std::string("/usr/share/unicode-data/Blocks.txt"),
                        fallback_dir + "/Blocks.txt"})
    if (exists(p)) return p;
  return "";
}

std::string utf8(uint32_t cp) {
  std::string s;
  if (cp < 0x80) s += (char)cp;
  else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) {
    s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
  } else {
    s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
    s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
  }
  return s;
}

uint32_t first_codepoint(const std::string &s) {
  if (s.empty()) return 0;
  unsigned char c = (unsigned char)s[0];
  int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
  if ((int)s.size() < n) return c;
  uint32_t cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
  for (int i = 1; i < n; i++) cp = (cp << 6) | ((unsigned char)s[i] & 0x3F);
  return cp;
}

} // namespace charmap
