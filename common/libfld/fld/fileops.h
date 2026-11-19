// Background file operations (copy / move / delete) with progress reporting,
// and the drag and drop rules shared by fl-files and fl-desktop
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

namespace fld {

// Paths from a text/uri-list or a plain list of paths (file:// URIs are
// decoded; other URIs, comments and paths that do not exist are left out).
std::vector<std::string> parse_uri_list(const std::string &text);
// The text/uri-list for paths (lines end with \r\n).
std::string make_uri_list(const std::vector<std::string> &paths);

enum DropAction { DROP_NONE, DROP_COPY, DROP_MOVE, DROP_TRASH };
// What a drop of `sources` into the folder `dest` does. `state` is the FLTK
// event state: Ctrl copies, Shift moves; otherwise files move within a file
// system and are copied to another one. Dropping on "trash:///" trashes them.
// DROP_NONE when there is nothing to do (all already in dest, or a folder
// would go into itself).
DropAction drop_action(const std::vector<std::string> &sources, const std::string &dest, int state);
const char *drop_action_name(DropAction a); // "Copy", "Move", ...

// Runs a file operation with the usual UI: asks about name conflicts, shows a
// progress window for long operations, reports errors. `done` is called in
// the FLTK thread when it has finished (also when it was cancelled; not when
// the user cancelled at the conflict question). Needs Fl::lock().
void run_file_op(FileOp::Kind kind, const std::vector<std::string> &sources, const std::string &dest,
                 std::function<void(FileOp *)> done = nullptr);
// Runs the operation a drop asks for (nothing for DROP_NONE).
void run_drop(DropAction a, const std::vector<std::string> &sources, const std::string &dest,
              std::function<void(FileOp *)> done = nullptr);

} // namespace fld
