// fl-media-player's own yt-dlp and QuickJS-ng: where they are, keeping them
// up to date (from GitHub, verified by SHA-256), and the command that
// resolves a video page. Settings: ~/.config/fldesktop/media-player.ini [Helpers].
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace webhelpers {

struct State {
  bool manage = true;          // download and update them ourselves
  long long checked = 0;       // time of the last successful check
  std::string ytdlp_version, qjs_version;
};
State load_state();
void save_state(const State &s);

std::string bin_dir();         // ~/.local/share/fldesktop/media-player/bin
// the yt-dlp to use (ours, else the system's), "" when there is none
std::string ytdlp_path();
// the command for a web page's stream (the right --js-runtimes)
std::vector<std::string> resolve_command(const std::string &page_url);

// ours are missing, or the last check is a day old
bool update_due();
bool updating();
// checks and downloads in a thread; done() runs in the FLTK thread with
// whether something was installed, and the versions or what went wrong
void update(std::function<void(bool ok, bool changed, const std::string &message)> done);

} // namespace webhelpers
