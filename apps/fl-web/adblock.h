// fl-web: ad and tracker blocking with WebKit content blockers
//
// Filter lists (EasyList, EasyPrivacy, others by address) are downloaded into
// the cache, converted to WebKit rules (adblock_rules.h) on a worker thread
// and compiled by WebKit into a content filter that every web view uses. The
// compiled filter is kept, so later starts load it at once; lists are fetched
// again when they are older than four days.
#pragma once
#include <functional>
#include <string>
#include <vector>

typedef struct _WebKitUserContentManager WebKitUserContentManager;

namespace web::adblock {

struct Settings {
  bool enabled = true;
  bool easylist = true;
  bool easyprivacy = true;
  std::vector<std::string> extra; // more list addresses
  std::vector<std::string> allow; // sites where nothing is blocked
};

// data_dir keeps the compiled filter, cache_dir the downloaded lists
void init(WebKitUserContentManager *ucm, const std::string &data_dir, const std::string &cache_dir);
// Applies new settings: turns blocking on or off, rebuilds the filter when the
// lists or allowed sites changed and downloads missing or old lists. `done`
// runs once the filter in use matches the settings.
void apply(const Settings &s, std::function<void()> done = {});
// downloads every list again
void update_now();
// "Lists updated 20 Nov 2026, 81 234 rules", "Downloading lists…", an error
std::string status();
// called when status() changes
extern std::function<void()> on_status;

// the WebKit rules for the given list files (for --adblock-convert)
std::string convert_files(const std::vector<std::string> &paths, size_t *rules, size_t *skipped);

} // namespace web::adblock
