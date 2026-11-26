// fldesktop common helpers
#pragma once
#include <functional>
#include <map>
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

// Physical memory in bytes (0 if unknown)
unsigned long long total_ram_bytes();
// The Wayland compositor of this session still accepts connections (true
// when WAYLAND_DISPLAY is not set: nothing to check). Lets background
// helpers end with the session.
bool wayland_display_alive();

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
// Compares file names the way people count: "file2" < "file10". Runs of
// digits compare by value, the rest case-insensitively; <0, 0 or >0.
int natural_compare(const std::string &a, const std::string &b);
bool starts_with(const std::string &s, const std::string &p);
bool ends_with(const std::string &s, const std::string &p);
std::vector<std::string> split(const std::string &s, char sep, bool keep_empty = false);
std::string join(const std::vector<std::string> &v, const std::string &sep);
std::string shell_quote(const std::string &s);
std::vector<std::string> shell_split(const std::string &cmd); // simple POSIX-ish word split
std::string human_size(unsigned long long bytes);
std::string format_time(time_t t, const char *fmt = "%Y-%m-%d %H:%M");
// Whether a strftime() format shows seconds (%S, %T, %r, %X, %c, %s, ...):
// a clock with this format needs updating every second, others every minute.
bool strftime_has_seconds(const std::string &fmt);
// Seconds until the text of strftime(fmt) can next change after now.
double strftime_next_change(const std::string &fmt, double now);
std::string uri_encode_path(const std::string &path);
std::string uri_decode(const std::string &s);

// Processes
// Starts argv detached from us (double fork + setsid). Returns false on failure.
// Starts a program detached (double fork). It gets the session environment
// as wm.ini [environment] has it now (see session_env_changes()).
bool spawn(const std::vector<std::string> &argv, const std::string &cwd = "");
bool spawn_shell(const std::string &cmdline, const std::string &cwd = "");
// Runs a command and captures stdout (blocking). Returns exit status or -1.
int run_capture(const std::vector<std::string> &argv, std::string &out);
// Like run_capture, writing `input` to the command's stdin first (passwords
// go this way, never in argv). With merge_stderr its stderr is captured too.
int run_capture_input(const std::vector<std::string> &argv, const std::string &input, std::string &out,
                      bool merge_stderr = false);
// Runs a command and calls on_line for each line of its stdout as it comes
// (without the newline). Blocking. Returns exit status or -1.
int run_lines(const std::vector<std::string> &argv, const std::function<void(const std::string &)> &on_line);
// KEY=value lines (optionally "export", quoted values) -> map
std::map<std::string, std::string> parse_env_text(const std::string &text);
// $XDG_RUNTIME_DIR/fldesktop-session.env: the environment variables fl-session
// started the session with (written by fl-session, used for "@inherit")
std::string session_env_snapshot_path();
// Changes wm.ini [environment] makes to the environment: (KEY, value); a
// value of "\x01" means unset.
std::vector<std::pair<std::string, std::string>> session_env_changes();
// Whether a process of this user with that name (/proc/<pid>/comm, at most
// 15 characters) runs.
bool process_running(const std::string &comm);
// Starts argv (spawn()) unless a process of this user named like argv[0]
// runs already. Returns false when starting it failed.
bool ensure_running(const std::vector<std::string> &argv);
// Finds an executable in $PATH.
std::string which(const std::string &prog);
// Preferred terminal emulator command (fl-term, then fallbacks)
std::string terminal_command();

// Session helpers
// Asks fl-wm ($FLWM_PID) to reload wm.ini. Returns false outside an fl-wm session.
bool reload_wm();
std::string wm_config_path();      // ~/.config/fldesktop/wm.ini
// Replaces the running program with a fresh copy of itself (same arguments).
void restart_self(int argc, char **argv);

} // namespace fld
