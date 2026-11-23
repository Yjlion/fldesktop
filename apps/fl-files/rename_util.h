// fl-files: batch rename (pattern, find and replace, letter case) and the
// order to rename in so that names can swap. No FLTK here (unit tested).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace frename {

enum class Case { Keep, Lower, Upper, Title };

struct Rule {
  // {name} the name without its extension, {ext} the extension with its dot,
  // {n} a counter, {n:3} a counter with three digits
  std::string pattern = "{name}{ext}";
  std::string find, replace; // applied after the pattern
  bool regex = false, ignore_case = false;
  Case letter_case = Case::Keep; // of the part before the extension
  int start = 1, step = 1;
};

// "photo.JPG" -> {"photo", ".JPG"}; ".bashrc" and "README" have none
std::pair<std::string, std::string> split_ext(const std::string &name);

// the new names, in the same order; err is set (and the result empty) on a bad regex
std::vector<std::string> apply(const std::vector<std::string> &names, const Rule &r, std::string *err = nullptr);

// what is wrong with a new name list: per name "" or a reason
// (empty, has a slash, "." or "..", the same as another new name)
std::vector<std::string> problems(const std::vector<std::string> &old_names, const std::vector<std::string> &new_names);

// the renames to do, in order, so that a -> b, b -> a works: names that
// another rename wants go through a temporary name first. Unchanged names are left out.
std::vector<std::pair<std::string, std::string>> plan(const std::vector<std::string> &old_names,
                                                      const std::vector<std::string> &new_names,
                                                      const std::string &temp_prefix);

} // namespace frename
