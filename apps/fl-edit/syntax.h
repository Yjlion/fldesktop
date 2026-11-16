// Small syntax highlighter for fl-edit (no FLTK dependency)
#pragma once
#include <string>

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
  HEADING = 'H', // markdown headings, ini sections
  FUNCTION = 'I',
};

enum Language { NONE, C_CPP, PYTHON, SHELL, JAVASCRIPT, RUST, GO, JAVA, INI, MARKDOWN, CMAKE, JSON, XML, CSS };

Language detect(const std::string &filename, const std::string &first_line);
const char *language_name(Language l);
// Fills `styles` (same length as `text`) with style characters.
void highlight(Language lang, const std::string &text, std::string &styles);

} // namespace syntax
