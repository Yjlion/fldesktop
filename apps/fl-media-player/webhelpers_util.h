// fl-media-player keeps its own yt-dlp and QuickJS-ng (YouTube's challenges
// need a JavaScript engine), downloaded from their GitHub releases: the
// distributions' yt-dlp is usually too old for the video sites. The plain
// parts of that, unit tested (no network, no FLTK).
#pragma once
#include <string>
#include <vector>

namespace webhelpers {

extern const char *const kYtdlpBase;     // .../releases/latest/download/
extern const char *const kQjsApi;        // the latest QuickJS-ng release (GitHub API)
extern const char *const kQjsDownload;   // where its assets must come from

// "qjs-linux-x86_64" for uname's machine, "" when QuickJS-ng has no build for it
std::string qjs_asset(const std::string &machine);
// the checksum of `file` in a SHA2-256SUMS text ("<sha256>  [*]<name>" lines), or ""
std::string sha256_in_sums(const std::string &sums, const std::string &file);
// From the GitHub release JSON: the tag, and the sha256 digest and download
// URL of the named asset. False if missing or not from QuickJS-ng's releases.
bool qjs_release_asset(const std::string &json, const std::string &asset, std::string &tag, std::string &sha256,
                       std::string &url);
bool is_sha256(const std::string &s);
// yt-dlp versions are dates (2025.11.12): a plain compare works
bool version_at_least(const std::string &version, const std::string &min);
// yt-dlp's --js-runtimes value: "" when yt-dlp finds one itself (deno) or
// there is none, or when yt-dlp is older than 2025.11 (no such option)
std::string js_runtime(bool have_deno, const std::string &managed_qjs, bool system_qjs,
                       const std::string &ytdlp_version);
// the command that finds the stream of a web page
std::vector<std::string> resolve_cmd(const std::string &ytdlp, const std::string &js_runtime,
                                     const std::string &page_url);

} // namespace webhelpers
