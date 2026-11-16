// fldesktop common helpers
#pragma once
#include <string>
#include <vector>
#include <ctime>
#include <sys/types.h>

namespace fld {

// Environment / XDG directories
std::string home_dir();
std::string getenv_or(const char *name, const std::string &fallback);
std::string config_home();                 // $XDG_CONFIG_HOME or ~/.config
std::string data_home();                   // $XDG_DATA_HOME or ~/.local/share
std::string cache_home();                  // $XDG_CACHE_HOME or ~/.cache
std::vector<std::string> data_dirs();      // data_home + $XDG_DATA_DIRS
std::vector<std::string> config_dirs();    // config_home + $XDG_CONFIG_DIRS
std::string fld_config_dir();              // ~/.config/fldesktop (created)
std::string user_dir(const char *key);     // XDG user dir, e.g. "DESKTOP"
std::string fld_data_dir();                // $FLD_DATADIR or the installed share/fldesktop

// Filesystem helpers
bool file_exists(const std::string &path);
bool is_dir(const std::string &path);
bool mkdir_p(const std::string &path, mode_t mode = 0755);
bool read_file(const std::string &path, std::string &out, size_t max = 64u << 20);
bool write_file(const std::string &path, const std::string &data);
std::string path_join(const std::string &a, const std::string &b);
std::string basename(const std::string &path);
std::string dirname(const std::string &path);
std::string extension(const std::string &path); // lower-case, no dot
std::string abs_path(const std::string &path);
std::string tilde(const std::string &path);      // /home/u/x -> ~/x
std::string expand_tilde(const std::string &path);
std::string unique_name(const std::string &dir, const std::string &name);
std::vector<std::string> list_dir(const std::string &dir, bool hidden = false);

// Strings
std::string trim(const std::string &s);
std::string lower(std::string s);
bool starts_with(const std::string &s, const std::string &p);
bool ends_with(const std::string &s, const std::string &p);
std::vector<std::string> split(const std::string &s, char sep, bool keep_empty = false);
std::string join(const std::vector<std::string> &v, const std::string &sep);
std::string shell_quote(const std::string &s);
std::vector<std::string> shell_split(const std::string &cmd); // simple POSIX-ish word split
std::string human_size(unsigned long long bytes);
std::string format_time(time_t t, const char *fmt = "%Y-%m-%d %H:%M");
std::string uri_encode_path(const std::string &path);
std::string uri_decode(const std::string &s);

// Processes
// Starts argv detached from us (double fork + setsid). Returns false on failure.
bool spawn(const std::vector<std::string> &argv, const std::string &cwd = "");
bool spawn_shell(const std::string &cmdline, const std::string &cwd = "");
// Runs a command and captures stdout (blocking). Returns exit status or -1.
int run_capture(const std::vector<std::string> &argv, std::string &out);
// Finds an executable in $PATH.
std::string which(const std::string &prog);
// Preferred terminal emulator command (fl-term, then fallbacks)
std::string terminal_command();

} // namespace fld
