// Moving a tab to another window, also of another process of the same program.
//
// The tab's state goes into a file in $XDG_RUNTIME_DIR/fldesktop/tabs; the
// drag carries "x-fld-tab:<file>". The window it is dropped on claims the
// file (an atomic rename, so only one can), and the source closes its tab
// once the file is gone. A file nobody claimed means "dropped outside": the
// program opens a new window with --adopt-tab <file>.
#pragma once
#include <string>

namespace fld {

extern const char *const kTabPayloadPrefix; // "x-fld-tab:"

std::string tab_dir(); // created (mode 0700) when missing
// Writes a tab's state; returns the file ("" on failure). Removes leftovers
// older than a minute.
std::string write_tab_file(const std::string &app, const std::string &state, const std::string &label);
// The file a drag's text names, when it is a tab payload for a file in tab_dir()
bool parse_tab_payload(const std::string &text, std::string &file);
// Takes the tab out of `file` when it is for `app`: only one caller succeeds.
// `pid`: the process it came from.
bool claim_tab_file(const std::string &file, const std::string &app, std::string &state, int *pid = nullptr);
// Puts a claimed state back (the target could not take it).
bool restore_tab_file(const std::string &file, const std::string &app, const std::string &state, int pid);
void discard_tab_file(const std::string &file);
// this program's own executable (for --adopt-tab), else `fallback`
std::string self_program(const std::string &fallback);

// Routes dropped tab payloads to the window's TabBar (fld::init_app installs it).
void install_tab_drop_hook();

} // namespace fld
