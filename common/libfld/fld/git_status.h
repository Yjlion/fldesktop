// Git state of files, for the marks in fl-files (emblems) and fl-edit (the
// gutter). The parsers are pure (unit tested, fuzzed); the helpers run git
// with argv (no shell) and give up quietly outside a repository.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace fld {
namespace git {

enum class State { Clean, Modified, Added, Deleted, Renamed, Untracked, Ignored, Conflicted };

// `git status --porcelain=v2 -z` output: path (relative to the top of the
// work tree) -> state. Renames are listed under their new path.
std::map<std::string, State> parse_status(const std::string &z);

// The state of a file or folder in a status map: a folder is Modified when
// anything below it is not clean (Conflicted wins, then Modified).
State state_of(const std::map<std::string, State> &status, const std::string &rel_path, bool is_dir);

// One run of changed lines in the working copy, from `git diff -U0`:
// lines are 1-based; a deletion marks the line after which lines went
// (count 0, at least line 1).
struct Hunk {
  enum Kind { Added, Changed, Deleted } kind;
  int line, count;
};
std::vector<Hunk> parse_diff(const std::string &diff);

// The top of the work tree that holds dir, or "" (git missing, not a repository).
std::string work_tree(const std::string &dir);
// The states under a work tree (git status there); empty on failure.
std::map<std::string, State> status(const std::string &top);
// The changed lines of a file against the index (git diff -U0).
std::vector<Hunk> diff_lines(const std::string &file);

}  // namespace git
}  // namespace fld
