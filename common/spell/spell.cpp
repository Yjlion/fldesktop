#include "spell.h"

#include <algorithm>
#include <cstdlib>
#include <enchant.h>

#include "fld/ini.h"
#include "fld/theme.h"

namespace spell {

std::vector<std::string> configured_languages() {
  fld::Ini d;
  d.load(fld::desktop_config_path());
  std::vector<std::string> v = split_languages(d.get("Spelling", "Languages", ""));
  if (v.empty()) {
    const char *l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    v.push_back(language_from_locale(l ? l : ""));
  }
  return v;
}

std::vector<std::string> installed_languages() {
  std::vector<std::string> v;
  EnchantBroker *b = enchant_broker_init();
  if (!b) return v;
  enchant_broker_list_dicts(b, [](const char *tag, const char *, const char *, const char *, void *ud) {
    auto *v = (std::vector<std::string> *)ud;
    if (std::find(v->begin(), v->end(), tag) == v->end()) v->push_back(tag);
  }, &v);
  enchant_broker_free(b);
  std::sort(v.begin(), v.end());
  return v;
}

Checker::Checker() { open(configured_languages()); }
Checker::Checker(const std::vector<std::string> &languages) { open(languages); }

void Checker::open(const std::vector<std::string> &languages) {
  EnchantBroker *b = enchant_broker_init();
  broker_ = b;
  if (!b) return;
  for (auto &l : languages) {
    std::string tag = l;
    // "de" when there is no "de_CH", and the other way round
    if (!enchant_broker_dict_exists(b, tag.c_str())) {
      std::string base = tag.substr(0, tag.find('_'));
      if (enchant_broker_dict_exists(b, base.c_str())) tag = base;
      else continue;
    }
    if (EnchantDict *d = enchant_broker_request_dict(b, tag.c_str())) {
      dicts_.push_back(d);
      langs_.push_back(tag);
    }
  }
}

Checker::~Checker() {
  EnchantBroker *b = (EnchantBroker *)broker_;
  for (void *d : dicts_) enchant_broker_free_dict(b, (EnchantDict *)d);
  if (b) enchant_broker_free(b);
}

bool Checker::check(const std::string &word) {
  if (dicts_.empty()) return true;
  auto it = cache_.find(word);
  if (it != cache_.end()) return it->second;
  bool good = false;
  for (void *d : dicts_)
    if (enchant_dict_check((EnchantDict *)d, word.c_str(), (ssize_t)word.size()) == 0) {
      good = true;
      break;
    }
  if (cache_.size() > 200000) cache_.clear();
  cache_[word] = good;
  return good;
}

std::vector<std::string> Checker::suggest(const std::string &word, size_t max) {
  std::vector<std::string> v;
  for (void *d : dicts_) {
    size_t n = 0;
    char **s = enchant_dict_suggest((EnchantDict *)d, word.c_str(), (ssize_t)word.size(), &n);
    for (size_t i = 0; i < n && v.size() < max; i++)
      if (std::find(v.begin(), v.end(), s[i]) == v.end()) v.push_back(s[i]);
    if (s) enchant_dict_free_string_list((EnchantDict *)d, s);
  }
  return v;
}

void Checker::add_to_dictionary(const std::string &word) {
  if (!dicts_.empty()) enchant_dict_add((EnchantDict *)dicts_[0], word.c_str(), (ssize_t)word.size());
  cache_[word] = true;
}

void Checker::ignore(const std::string &word) {
  for (void *d : dicts_) enchant_dict_add_to_session((EnchantDict *)d, word.c_str(), (ssize_t)word.size());
  cache_[word] = true;
}

} // namespace spell
