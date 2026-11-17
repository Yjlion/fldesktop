// File and folder search for fl-files (no FLTK dependency)
#pragma once
#include <atomic>
#include <ctime>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace search {

struct Query {
  std::string root;            // folder to search in
  std::string name;            // substring, or a glob when it contains * ? [
  bool name_regex = false;     // name is a regular expression
  bool match_case = false;
  std::string content;         // text the file must contain (files only)
  enum Kind { ANY, FILES, FOLDERS } kind = ANY;
  long long min_size = -1, max_size = -1; // bytes; -1: no limit (files only)
  time_t newer_than = 0;       // modified at or after this time; 0: any
  bool hidden = false;         // descend into / report hidden entries
  bool follow_links = false;   // descend into symlinked folders
  int max_depth = -1;          // -1: unlimited; 0: only the root's entries
  size_t max_results = 50000;
};

// Name matching as the search does it. Returns false for an invalid regex.
class NameMatcher {
public:
  explicit NameMatcher(const Query &q);
  bool valid() const { return valid_; }
  bool operator()(const std::string &name) const;

private:
  enum Mode { ALL, SUBSTRING, GLOB, REGEX } mode_ = ALL;
  std::string pattern_;
  bool icase_ = true, valid_ = true;
  std::regex re_;
};

// True when the file contains `needle` (case-insensitive unless match_case).
// Binary files (a NUL byte in the first 8 KiB) and files over max_bytes never match.
bool file_contains(const std::string &path, const std::string &needle, bool match_case,
                   unsigned long long max_bytes = 64ull << 20);

// Runs a query on a worker thread. Results are collected and taken by the UI.
class Search {
public:
  Search() = default;
  ~Search();
  Search(const Search &) = delete;
  Search &operator=(const Search &) = delete;

  // Starts (or restarts) a search. Returns false if the name regex is invalid.
  bool start(const Query &q);
  void cancel();             // stops and joins the worker
  bool running() const { return running_; }
  bool truncated() const { return truncated_; } // stopped at max_results
  // Moves new results (paths) into `out`; returns how many were added.
  size_t take(std::vector<std::string> &out);
  size_t found() const { return found_; }
  size_t scanned() const { return scanned_; }
  std::string current_dir();

  // Runs a query synchronously (for tests and small searches).
  static std::vector<std::string> run(const Query &q);

private:
  std::thread worker_;
  std::atomic<bool> stop_{false}, running_{false}, truncated_{false};
  std::atomic<size_t> found_{0}, scanned_{0};
  std::mutex mtx_;
  std::vector<std::string> pending_;
  std::string current_;
  void work(Query q);
};

} // namespace search
