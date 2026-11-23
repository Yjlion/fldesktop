// fl-edit: the Language Server Protocol, without the process and the UI:
// message framing, the requests fl-edit sends, and reading the answers.
// Positions are (line, UTF-16 column) in LSP and (line, byte column) here.
#pragma once
#include "fld/json.h"
#include <string>
#include <vector>

namespace lsp {

// "Content-Length: N\r\n\r\n" + body
std::string frame(const std::string &body);

// collects bytes from the server and hands out whole messages
struct Reader {
  std::string buf;
  void feed(const char *data, size_t n) { buf.append(data, n); }
  bool next(std::string &body); // false when no whole message is there yet
};

std::string path_to_uri(const std::string &path);
std::string uri_to_path(const std::string &uri); // "" when not a file:// URI

// between a byte offset in a line and its UTF-16 column
int utf16_col(const std::string &line, int byte_col);
int byte_col(const std::string &line, int utf16_col);

// the language id LSP wants for a fl-edit language name ("C++" -> "cpp")
std::string language_id(const std::string &lang_name);
// the server to start for a language id, e.g. {"clangd"}; {} when none is known
std::vector<std::string> default_server(const std::string &language_id);

// requests and notifications (whole JSON-RPC messages, unframed)
std::string initialize(int id, int pid, const std::string &root_uri);
std::string initialized();
std::string did_open(const std::string &uri, const std::string &language_id, int version, const std::string &text);
std::string did_change(const std::string &uri, int version, const std::string &text); // full text
std::string did_save(const std::string &uri);
std::string did_close(const std::string &uri);
std::string completion(int id, const std::string &uri, int line, int col16);
std::string definition(int id, const std::string &uri, int line, int col16);
std::string shutdown(int id);
std::string exit_notification();

struct Diagnostic {
  int line = 0, col = 0, end_line = 0, end_col = 0; // UTF-16 columns
  int severity = 1;                                  // 1 error, 2 warning, 3 info, 4 hint
  std::string message, source;
};

struct Location {
  std::string path;
  int line = 0, col = 0; // UTF-16 column
};

struct CompletionItem {
  std::string label, insert, detail;
};

// a message from the server, sorted out
struct Message {
  enum Kind { Other, Response, Diagnostics, ServerRequest } kind = Other;
  int id = -1;              // Response, ServerRequest
  fld::Json result, error;  // Response
  std::string method;       // ServerRequest and notifications
  std::string uri;          // Diagnostics
  std::vector<Diagnostic> diagnostics;
};
Message parse(const std::string &body);

std::vector<CompletionItem> parse_completion(const fld::Json &result);
std::vector<Location> parse_locations(const fld::Json &result);

// the answer to a request from the server that fl-edit does not handle
std::string reply_null(int id);

} // namespace lsp
