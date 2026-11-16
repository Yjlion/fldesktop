// Background file operations (copy / move / delete) with progress reporting
#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct FileOp {
  enum Kind { COPY, MOVE, DELETE, TRASH } kind;
  std::vector<std::string> sources;
  std::string dest_dir;           // COPY / MOVE
  enum Conflict { KEEP_BOTH, REPLACE, SKIP } conflict = KEEP_BOTH;

  // progress (written by the worker, read by the UI)
  std::atomic<unsigned long long> done_bytes{0}, total_bytes{0};
  std::atomic<int> done_items{0};
  std::atomic<bool> cancel{false}, finished{false};
  std::mutex mtx;
  std::string current;            // file being processed
  std::vector<std::string> errors;

  // Runs the operation in a thread; `on_done` is called in the FLTK thread.
  void start(std::function<void(FileOp *)> on_done);
  ~FileOp();

private:
  std::thread worker_;
  std::function<void(FileOp *)> on_done_;
  void run();
  void set_current(const std::string &s);
  void error(const std::string &e);
  static void awake_done(void *data);
};

// Total size of files below path (does not follow symlinks).
unsigned long long tree_size(const std::string &path, std::atomic<bool> *cancel = nullptr,
                             unsigned long long *files = nullptr, unsigned long long *dirs = nullptr);
