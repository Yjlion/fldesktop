// Small syntax highlighter for fl-edit (no FLTK dependency)
#pragma once
#include <string>
#include <vector>

namespace syntax {

// Style characters, used as Fl_Text_Display style buffer entries
enum : char {
  PLAIN = 'A',
  COMMENT = 'B',
  STRING = 'C',
  KEYWORD = 'D',
  NUMBER = 'E',
  PREPROC = 'F',
  TYPE = 'G',
  HEADING = 'H', // markdown headings, ini sections, diff file headers
  FUNCTION = 'I',
  ADDED = 'J',   // diff: added lines
  REMOVED = 'K', // diff: removed lines
  BRACKET = 'L', // matching bracket (set by the editor, not by highlight())
};
const int STYLE_COUNT = 12;

enum Language {
  NONE, C_CPP, PYTHON, SHELL, JAVASCRIPT, RUST, GO, JAVA, INI, MARKDOWN, CMAKE, JSON, XML, CSS,
  TYPESCRIPT, PHP, RUBY, LUA, PERL, SQL, YAML, TOML, MAKEFILE, DIFF, HTML, KOTLIN, CSHARP, MESON,
  LANGUAGE_COUNT
};

Language detect(const std::string &filename, const std::string &first_line);
const char *language_name(Language l);
// Languages sorted by name (without NONE), for menus.
std::vector<Language> all_languages();
// Fills `styles` (same length as `text`) with style characters.
void highlight(Language lang, const std::string &text, std::string &styles);

// Position of the bracket matching the one at `pos` (one of ()[]{}), skipping
// brackets inside comments and strings according to `styles` (may be empty).
// Returns -1 when there is no bracket at pos or no match.
long match_bracket(const std::string &text, const std::string &styles, size_t pos);

// Color themes. Colors are 0xRRGGBB.
struct Theme {
  const char *id, *name;
  bool dark;
  unsigned bg, line_bg, line_fg, selection, cursor, bracket_bg;
  unsigned colors[STYLE_COUNT]; // indexed by style - 'A'
  unsigned char bold[STYLE_COUNT], italic[STYLE_COUNT];
};
const std::vector<Theme> &themes();
// Finds a theme by id; "auto" (or an unknown id) picks the default light or
// dark theme depending on `dark`.
const Theme &theme(const std::string &id, bool dark);

} // namespace syntax
