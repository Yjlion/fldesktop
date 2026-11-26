// fl-web logic without WebKit (unit tested): what the address bar means,
// bookmarks and history files.
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace web {

// Search engines: name and URL template (%s is the query).
struct SearchEngine {
  const char *name, *url;
};
const std::vector<SearchEngine> &search_engines();
// The template for a name (or a template itself); DuckDuckGo when unknown.
std::string search_template(const std::string &name_or_template);

// What to load for text typed into the address bar: URLs and file paths as
// they are (adding https:// to host names), anything else is a search.
std::string url_from_input(const std::string &text, const std::string &search_tmpl);
// Percent-encodes a search query (spaces become +).
std::string encode_query(const std::string &q);
// The host part of a URL ("" when none).
std::string url_host(const std::string &url);
// A title for a page without one: the host, or the file name.
std::string fallback_title(const std::string &url);

struct Bookmark {
  std::string url, title;
};
// url<TAB>title lines
std::vector<Bookmark> load_bookmarks(const std::string &path);
bool save_bookmarks(const std::string &path, const std::vector<Bookmark> &b);

struct HistoryEntry {
  time_t time = 0;
  std::string url, title;
};
// time<TAB>url<TAB>title lines, oldest first
std::vector<HistoryEntry> load_history(const std::string &path);
// Appends a visit; the file is rewritten (deduplicated, at most `keep`
// entries) when it has grown to twice that.
bool add_history(const std::string &path, const HistoryEntry &e, size_t keep = 5000);
// Newest first, one entry per URL, matching `filter` (in URL or title,
// case-insensitive) when not empty.
std::vector<HistoryEntry> history_view(const std::vector<HistoryEntry> &all, const std::string &filter,
                                       size_t max = 500);

// Completions for the address bar: bookmarks, then the most visited history
// entries whose URL or title contains the text.
std::vector<Bookmark> suggestions(const std::string &text, const std::vector<Bookmark> &bookmarks,
                                  const std::vector<HistoryEntry> &history, size_t max = 8);

// Strips tabs and newlines (for the files above).
std::string one_line(const std::string &s);

// ---- saved logins -------------------------------------------------------------
// They live in the Secret Service with these attributes.
extern const char *const kLoginSchema; // xdg:schema
// "https://example.org:8443" for a page address; "" for other schemes
std::string login_origin(const std::string &url);
// a page script: posts {origin, user, pass} to the flwebLogin message handler
// when a form with a filled password field is sent
const char *login_capture_script();
// reads such a message (JSON); false when it is not one
bool parse_login_message(const std::string &json, std::string &origin, std::string &user, std::string &pass);
// a page script that fills the first password form (and the user name before
// it) unless the password field already holds something; returns 1 when it did
std::string login_fill_script(const std::string &user, const std::string &pass);

// Saved passwords exported by Firefox (url,username,password,...) or
// Chrome/Chromium (name,url,username,password,...) as CSV
struct CsvLogin {
  std::string origin, user, pass;
};
std::vector<CsvLogin> parse_password_csv(const std::string &text, std::string *err = nullptr);
// one CSV record's fields (RFC 4180 quoting); pos moves past the record
std::vector<std::string> csv_record(const std::string &text, size_t &pos);

// A userscript (Greasemonkey style): the // ==UserScript== header
struct UserScript {
  std::string name;
  std::vector<std::string> matches, excludes; // URL patterns (@match/@include, @exclude)
  bool at_start = false;                      // @run-at document-start
  std::string source;
};
bool parse_userscript(const std::string &text, UserScript &out);

} // namespace web
