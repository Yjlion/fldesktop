// File open / save dialog (used by xdg-desktop-portal-fl, usable by apps).
//
// The dialog is asynchronous: show_file_dialog() opens it and calls `done`
// once (FLTK thread) with the result. run_file_dialog() waits for it.
#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

class Fl_Double_Window;

namespace fld {

struct FileFilter {
  std::string name;
  std::vector<std::string> globs; // "*.png" (case-insensitive)
  std::vector<std::string> mimes; // "image/png", "image/*"
  bool matches(const std::string &filename) const;
};

struct FileChoice {
  std::string id, label;
  std::vector<std::pair<std::string, std::string>> options; // id, label; empty = a check box
  std::string selected;                                     // option id, or "true"/"false"
};

struct FileDialogOptions {
  enum Mode { OPEN, SAVE, SAVE_FILES /* pick a folder for several files */ } mode = OPEN;
  std::string title;
  std::string accept_label;            // default: Open / Save / Select
  bool multiple = false;               // OPEN: several files
  bool directory = false;              // OPEN: folders instead of files
  std::string folder;                  // start folder (default: home or last used)
  std::string name;                    // SAVE: proposed file name
  std::vector<FileFilter> filters;
  int current_filter = -1;             // index into filters (-1: the first)
  std::vector<FileChoice> choices;
  std::vector<std::string> files;      // SAVE_FILES: names that will be written (shown, checked for clashes)
};

struct FileDialogResult {
  bool ok = false;
  std::vector<std::string> paths;      // absolute paths
  int filter = -1;                     // filter in use when accepted
  std::vector<std::pair<std::string, std::string>> choices; // id, selected
};

// Opens the dialog. The returned window belongs to the dialog; call
// cancel_file_dialog() with it to close it from outside (done gets ok=false).
Fl_Double_Window *show_file_dialog(const FileDialogOptions &opt,
                                   std::function<void(const FileDialogResult &)> done);
void cancel_file_dialog(Fl_Double_Window *dialog);
FileDialogResult run_file_dialog(const FileDialogOptions &opt);

// Bookmarked folders: ~/.config/gtk-3.0/bookmarks (file:// URIs), as label, path.
std::vector<std::pair<std::string, std::string>> file_bookmarks();

} // namespace fld
