// fl-web: Adblock Plus filter lists to WebKit content blocker rules
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace web {

struct AdblockResult {
  std::string json;   // a WebKit content rule list (a JSON array)
  size_t rules = 0;   // rules in json
  size_t skipped = 0; // filters WebKit cannot express (regular expressions, scriptlets, ...)
};

// Converts filter lists in Adblock Plus syntax (EasyList, EasyPrivacy, ...).
// Network filters become "block" rules, element hiding "css-display-none"
// rules, exceptions "ignore-previous-rules". Hosts in `allow` (per-site
// "don't block here") end the list with a rule that undoes everything on
// pages of those sites. `max_rules` caps the output (WebKit refuses lists
// with more than 150 000 rules); exceptions and `allow` are always kept.
AdblockResult abp_to_webkit(const std::vector<std::string> &lists, const std::vector<std::string> &allow,
                            bool cosmetic = true, size_t max_rules = 140000);

// The url-filter regular expression for a filter's pattern (without options),
// e.g. "||ads.example^" -> "^[^:]+://+([^:/]+\\.)?ads\\.example[^-_.%A-Za-z0-9]".
// Empty when WebKit cannot express it.
std::string abp_url_filter(const std::string &pattern);

// The rule's domain for if-domain / unless-domain: "*example.com" (the site
// and its subdomains), or empty for a domain WebKit would reject.
std::string abp_domain(const std::string &domain);

// Appends the per-site "don't block here" rules for `allow` to a list made
// by abp_to_webkit (without an allowlist), as abp_to_webkit(lists, allow)
// would have: the base list can be kept and reused when only `allow` changes.
void abp_append_allow(AdblockResult &r, const std::vector<std::string> &allow);

// 64-bit FNV-1a
uint64_t abp_fnv1a(const std::string &s, uint64_t h = 1469598103934665603ull);

// A cache key for `fields`: 16 hex digits, safe to store in a file name or a
// one-line ini value (the fields themselves may hold any bytes)
std::string abp_key(const std::vector<std::string> &fields);

} // namespace web
