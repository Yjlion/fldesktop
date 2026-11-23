#include "lsp_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <strings.h>

using fld::Json;

namespace lsp {

std::string frame(const std::string &body) {
  return "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

bool Reader::next(std::string &body) {
  size_t end = buf.find("\r\n\r\n");
  if (end == std::string::npos) return false;
  long len = -1;
  size_t p = 0;
  while (p < end) {
    size_t eol = buf.find("\r\n", p);
    if (eol == std::string::npos || eol > end) eol = end;
    std::string h = buf.substr(p, eol - p);
    if (h.size() > 15 && strncasecmp(h.c_str(), "Content-Length:", 15) == 0) len = atol(h.c_str() + 15);
    p = eol + 2;
  }
  if (len < 0) { // a broken header: drop it
    buf.erase(0, end + 4);
    return false;
  }
  if (buf.size() < end + 4 + (size_t)len) return false;
  body = buf.substr(end + 4, (size_t)len);
  buf.erase(0, end + 4 + (size_t)len);
  return true;
}

std::string path_to_uri(const std::string &path) {
  static const char *hex = "0123456789ABCDEF";
  std::string out = "file://";
  for (unsigned char c : path) {
    if (isalnum(c) || strchr("/-_.~", c)) out += (char)c;
    else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string uri_to_path(const std::string &uri) {
  if (uri.compare(0, 7, "file://") != 0) return "";
  std::string out;
  for (size_t i = 7; i < uri.size(); i++) {
    if (uri[i] == '%' && i + 2 < uri.size() && isxdigit((unsigned char)uri[i + 1]) && isxdigit((unsigned char)uri[i + 2])) {
      out += (char)strtol(uri.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else out += uri[i];
  }
  return out;
}

// the length of the UTF-8 sequence that starts with c
static int seq_len(unsigned char c) { return c < 0x80 ? 1 : c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1; }

int utf16_col(const std::string &line, int byte_col) {
  int col = 0;
  for (int i = 0; i < byte_col && i < (int)line.size();) {
    int n = seq_len((unsigned char)line[(size_t)i]);
    col += n == 4 ? 2 : 1; // outside the BMP: a surrogate pair
    i += n;
  }
  return col;
}

int byte_col(const std::string &line, int col16) {
  int col = 0, i = 0;
  while (i < (int)line.size() && line[(size_t)i] != '\n' && col < col16) {
    int n = seq_len((unsigned char)line[(size_t)i]);
    col += n == 4 ? 2 : 1;
    i += n;
  }
  return std::min(i, (int)line.size());
}

std::string language_id(const std::string &name) {
  static const struct { const char *name, *id; } map[] = {
      {"C/C++", "cpp"},        {"Python", "python"}, {"Shell", "shellscript"}, {"JavaScript", "javascript"},
      {"TypeScript", "typescript"}, {"Rust", "rust"}, {"Go", "go"},          {"Java", "java"},
      {"Markdown", "markdown"}, {"CMake", "cmake"},   {"JSON", "json"},        {"XML", "xml"},
      {"CSS", "css"},          {"PHP", "php"},       {"Ruby", "ruby"},        {"Lua", "lua"},
      {"Perl", "perl"},        {"SQL", "sql"},       {"YAML", "yaml"},        {"TOML", "toml"},
      {"HTML", "html"},        {"Kotlin", "kotlin"}, {"C#", "csharp"},        {"Meson", "meson"},
  };
  for (auto &m : map)
    if (name == m.name) return m.id;
  return "";
}

std::vector<std::string> default_server(const std::string &id) {
  if (id == "cpp" || id == "c") return {"clangd"};
  if (id == "python") return {"pylsp"};
  if (id == "rust") return {"rust-analyzer"};
  if (id == "go") return {"gopls"};
  if (id == "javascript" || id == "typescript") return {"typescript-language-server", "--stdio"};
  if (id == "shellscript") return {"bash-language-server", "start"};
  if (id == "lua") return {"lua-language-server"};
  if (id == "yaml") return {"yaml-language-server", "--stdio"};
  if (id == "html") return {"vscode-html-language-server", "--stdio"};
  if (id == "css") return {"vscode-css-language-server", "--stdio"};
  if (id == "json") return {"vscode-json-language-server", "--stdio"};
  if (id == "cmake") return {"cmake-language-server"};
  if (id == "php") return {"intelephense", "--stdio"};
  if (id == "ruby") return {"solargraph", "stdio"};
  if (id == "toml") return {"taplo", "lsp", "stdio"};
  return {};
}

static Json request(int id, const char *method, const Json &params) {
  Json j = Json::object();
  j.set("jsonrpc", "2.0");
  j.set("id", id);
  j.set("method", method);
  j.set("params", params);
  return j;
}

static Json notification(const char *method, const Json &params) {
  Json j = Json::object();
  j.set("jsonrpc", "2.0");
  j.set("method", method);
  j.set("params", params);
  return j;
}

static Json doc_id(const std::string &uri) {
  Json t = Json::object();
  t.set("uri", uri);
  return t;
}

std::string initialize(int id, int pid, const std::string &root_uri) {
  Json p = Json::object();
  p.set("processId", pid);
  p.set("rootUri", root_uri.empty() ? Json() : Json(root_uri));
  Json info = Json::object();
  info.set("name", "fl-edit");
  p.set("clientInfo", info);
  Json caps = Json::object(), td = Json::object(), comp = Json::object(), item = Json::object();
  item.set("snippetSupport", false);
  comp.set("completionItem", item);
  td.set("completion", comp);
  Json diag = Json::object();
  diag.set("relatedInformation", false);
  td.set("publishDiagnostics", diag);
  Json sync = Json::object();
  sync.set("didSave", true);
  td.set("synchronization", sync);
  Json def = Json::object();
  def.set("linkSupport", false);
  td.set("definition", def);
  caps.set("textDocument", td);
  Json general = Json::object(), enc = Json::array();
  enc.push("utf-16");
  general.set("positionEncodings", enc);
  caps.set("general", general);
  p.set("capabilities", caps);
  if (!root_uri.empty()) {
    Json folders = Json::array(), f = Json::object();
    f.set("uri", root_uri);
    f.set("name", root_uri.substr(root_uri.rfind('/') + 1));
    folders.push(f);
    p.set("workspaceFolders", folders);
  }
  return request(id, "initialize", p).dump();
}

std::string initialized() { return notification("initialized", Json::object()).dump(); }

std::string did_open(const std::string &uri, const std::string &language_id, int version, const std::string &text) {
  Json td = doc_id(uri);
  td.set("languageId", language_id);
  td.set("version", version);
  td.set("text", text);
  Json p = Json::object();
  p.set("textDocument", td);
  return notification("textDocument/didOpen", p).dump();
}

std::string did_change(const std::string &uri, int version, const std::string &text) {
  Json td = doc_id(uri);
  td.set("version", version);
  Json ch = Json::array(), c = Json::object();
  c.set("text", text);
  ch.push(c);
  Json p = Json::object();
  p.set("textDocument", td);
  p.set("contentChanges", ch);
  return notification("textDocument/didChange", p).dump();
}

std::string did_save(const std::string &uri) {
  Json p = Json::object();
  p.set("textDocument", doc_id(uri));
  return notification("textDocument/didSave", p).dump();
}

std::string did_close(const std::string &uri) {
  Json p = Json::object();
  p.set("textDocument", doc_id(uri));
  return notification("textDocument/didClose", p).dump();
}

static Json position_params(const std::string &uri, int line, int col16) {
  Json p = Json::object(), pos = Json::object();
  p.set("textDocument", doc_id(uri));
  pos.set("line", line);
  pos.set("character", col16);
  p.set("position", pos);
  return p;
}

std::string completion(int id, const std::string &uri, int line, int col16) {
  return request(id, "textDocument/completion", position_params(uri, line, col16)).dump();
}

std::string definition(int id, const std::string &uri, int line, int col16) {
  return request(id, "textDocument/definition", position_params(uri, line, col16)).dump();
}

std::string shutdown(int id) {
  Json j = Json::object();
  j.set("jsonrpc", "2.0");
  j.set("id", id);
  j.set("method", "shutdown");
  return j.dump();
}

std::string exit_notification() {
  Json j = Json::object();
  j.set("jsonrpc", "2.0");
  j.set("method", "exit");
  return j.dump();
}

std::string reply_null(int id) {
  Json j = Json::object();
  j.set("jsonrpc", "2.0");
  j.set("id", id);
  j.set("result", Json());
  return j.dump();
}

static int num(const Json &j) { return (int)j.as_number(); }

Message parse(const std::string &body) {
  Message m;
  Json j;
  if (!Json::parse(body, j) || !j.is_object()) return m;
  bool has_id = j.has("id") && j["id"].type() == Json::Number;
  if (has_id) m.id = num(j["id"]);
  if (j.has("method")) {
    m.method = j["method"].as_string();
    if (has_id) {
      m.kind = Message::ServerRequest;
      return m;
    }
    if (m.method == "textDocument/publishDiagnostics") {
      m.kind = Message::Diagnostics;
      const Json &p = j["params"];
      m.uri = p["uri"].as_string();
      for (auto &d : p["diagnostics"].items()) {
        Diagnostic x;
        const Json &r = d["range"];
        x.line = num(r["start"]["line"]);
        x.col = num(r["start"]["character"]);
        x.end_line = num(r["end"]["line"]);
        x.end_col = num(r["end"]["character"]);
        x.severity = d.has("severity") ? num(d["severity"]) : 1;
        x.message = d["message"].as_string();
        x.source = d["source"].as_string();
        m.diagnostics.push_back(x);
      }
    }
    return m;
  }
  if (has_id) {
    m.kind = Message::Response;
    m.result = j["result"];
    m.error = j["error"];
  }
  return m;
}

std::vector<CompletionItem> parse_completion(const Json &result) {
  // CompletionItem[] or CompletionList {items}
  const Json &list = result.is_object() ? result["items"] : result;
  std::vector<CompletionItem> out;
  for (auto &it : list.items()) {
    CompletionItem c;
    c.label = it["label"].as_string();
    c.detail = it["detail"].as_string();
    if (it["textEdit"].is_object()) c.insert = it["textEdit"]["newText"].as_string();
    else if (it["insertText"].is_string()) c.insert = it["insertText"].as_string();
    else c.insert = c.label;
    // without snippet support some servers still send "foo($1)": keep the name
    size_t d = c.insert.find('$');
    if (d != std::string::npos && it["insertTextFormat"].as_number() == 2) c.insert = c.insert.substr(0, d);
    // clangd puts a bullet or a space in front of labels
    while (!c.label.empty() && (c.label[0] == ' ' || (unsigned char)c.label[0] == 0xE2)) {
      if (c.label[0] == ' ') c.label.erase(0, 1);
      else if (c.label.size() >= 3) c.label.erase(0, 3);
      else break;
    }
    if (!c.label.empty()) out.push_back(c);
  }
  return out;
}

std::vector<Location> parse_locations(const Json &result) {
  std::vector<Location> out;
  auto one = [&](const Json &l) {
    // Location {uri, range} or LocationLink {targetUri, targetSelectionRange}
    std::string uri = l.has("targetUri") ? l["targetUri"].as_string() : l["uri"].as_string();
    const Json &r = l.has("targetSelectionRange") ? l["targetSelectionRange"] : l["range"];
    Location x;
    x.path = uri_to_path(uri);
    x.line = num(r["start"]["line"]);
    x.col = num(r["start"]["character"]);
    if (!x.path.empty()) out.push_back(x);
  };
  if (result.is_array())
    for (auto &l : result.items()) one(l);
  else if (result.is_object())
    one(result);
  return out;
}

} // namespace lsp
