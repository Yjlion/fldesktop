// Spell checking through enchant-2 (Hunspell, Nuspell, Aspell dictionaries).
// The languages come from desktop.ini [Spelling] Languages (Settings >
// Language & Region), else from the locale. Words are checked against all
// of them; the personal word list is enchant's.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "spell_util.h"

namespace spell {

class Checker {
public:
  Checker();  // the configured languages
  explicit Checker(const std::vector<std::string> &languages);
  ~Checker();
  Checker(const Checker &) = delete;
  Checker &operator=(const Checker &) = delete;

  bool ok() const { return !dicts_.empty(); } // a dictionary is there
  const std::vector<std::string> &languages() const { return langs_; }
  bool check(const std::string &word);
  std::vector<std::string> suggest(const std::string &word, size_t max = 8);
  void add_to_dictionary(const std::string &word); // the personal word list
  void ignore(const std::string &word);            // for this session

private:
  void *broker_ = nullptr;
  std::vector<void *> dicts_;
  std::vector<std::string> langs_;
  std::unordered_map<std::string, bool> cache_;
  void open(const std::vector<std::string> &languages);
};

// the configured languages (desktop.ini, else the locale)
std::vector<std::string> configured_languages();
// the dictionaries installed ("en_US", "de_DE", ...)
std::vector<std::string> installed_languages();

} // namespace spell
