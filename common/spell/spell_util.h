// Spell checking: finding the words to check (plain logic, unit tested).
#pragma once
#include <string>
#include <vector>

namespace spell {

struct Word {
  size_t start, len; // bytes
};
// The words of text[from, to) worth checking: letters (UTF-8 letters too)
// with inner apostrophes. Skipped: single letters, words with digits or '_',
// camelCase and ALLCAPS, and whole tokens that are URLs, e-mail addresses
// or paths.
std::vector<Word> words(const std::string &text, size_t from = 0, size_t to = std::string::npos);
// "en_US.UTF-8" -> "en_US"; "C" / "POSIX" / "" -> "en_US"
std::string language_from_locale(const std::string &locale);
// desktop.ini [Spelling] Languages=en_US,de_DE -> {"en_US", "de_DE"}
std::vector<std::string> split_languages(const std::string &list);

} // namespace spell
