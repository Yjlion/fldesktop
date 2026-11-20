// fl-web: Adblock Plus filter lists to WebKit content blocker rules
#pragma once
#include <cstddef>
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

} // namespace web
