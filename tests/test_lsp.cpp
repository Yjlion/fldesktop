// fl-edit: the language server protocol pieces
#include "lsp_util.h"
#include "test.h"

using namespace lsp;

int main() {
  std::string f = frame("{}");
  CHECK_EQ(f, std::string("Content-Length: 2\r\n\r\n{}"));
  Reader r;
  std::string a = frame("{\"id\":1}"), b = frame("{\"id\":2}"), body;
  std::string both = a + b;
  r.feed(both.data(), 5);
  CHECK(!r.next(body));
  r.feed(both.data() + 5, both.size() - 5);
  CHECK(r.next(body));
  CHECK_EQ(body, std::string("{\"id\":1}"));
  CHECK(r.next(body));
  CHECK_EQ(body, std::string("{\"id\":2}"));
  CHECK(!r.next(body));
  std::string extra = "Content-Type: x\r\nContent-Length: 3\r\n\r\n[1]";
  r.feed(extra.data(), extra.size());
  CHECK(r.next(body));
  CHECK_EQ(body, std::string("[1]"));

  CHECK_EQ(path_to_uri("/home/a b/ü.c"), std::string("file:///home/a%20b/%C3%BC.c"));
  CHECK_EQ(uri_to_path("file:///home/a%20b/%C3%BC.c"), std::string("/home/a b/ü.c"));
  CHECK_EQ(uri_to_path("https://x"), std::string(""));

  std::string line = "aü😀b";
  CHECK_EQ(utf16_col(line, 1), 1);
  CHECK_EQ(utf16_col(line, 3), 2);
  CHECK_EQ(utf16_col(line, 7), 4);
  CHECK_EQ(byte_col(line, 4), 7);
  CHECK_EQ(byte_col(line, 99), (int)line.size());

  CHECK_EQ(language_id("C/C++"), std::string("cpp"));
  CHECK(default_server("cpp") == std::vector<std::string>({"clangd"}));
  CHECK(default_server("nope").empty());

  fld::Json j;
  CHECK(fld::Json::parse(initialize(1, 42, "file:///p"), j));
  CHECK_EQ(j["method"].as_string(), std::string("initialize"));
  CHECK_EQ((int)j["params"]["processId"].as_number(), 42);
  CHECK(fld::Json::parse(did_change("file:///p/a.c", 3, "x\n"), j));
  CHECK_EQ(j["params"]["contentChanges"].items()[0]["text"].as_string(), std::string("x\n"));
  CHECK(j["id"].is_null());
  CHECK(fld::Json::parse(completion(7, "file:///a", 2, 5), j));
  CHECK_EQ((int)j["params"]["position"]["character"].as_number(), 5);

  Message m = parse("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{\"uri\":\"file:///a.c\","
                    "\"diagnostics\":[{\"range\":{\"start\":{\"line\":3,\"character\":4},\"end\":{\"line\":3,\"character\":9}},"
                    "\"severity\":2,\"message\":\"unused variable\",\"source\":\"clang\"}]}}");
  CHECK(m.kind == Message::Diagnostics);
  CHECK_EQ(m.diagnostics.size(), (size_t)1);
  CHECK_EQ(m.diagnostics[0].line, 3);
  CHECK_EQ(m.diagnostics[0].end_col, 9);
  CHECK_EQ(m.diagnostics[0].severity, 2);
  m = parse("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"window/workDoneProgress/create\",\"params\":{}}");
  CHECK(m.kind == Message::ServerRequest);
  CHECK_EQ(m.id, 5);
  m = parse("{\"jsonrpc\":\"2.0\",\"id\":7,\"result\":{\"isIncomplete\":false,\"items\":[{\"label\":\" printf\","
            "\"insertText\":\"printf\",\"detail\":\"int\"},{\"label\":\"pr\",\"textEdit\":{\"newText\":\"print($1)\","
            "\"range\":{}},\"insertTextFormat\":2}]}}");
  CHECK(m.kind == Message::Response);
  auto items = parse_completion(m.result);
  CHECK_EQ(items.size(), (size_t)2);
  CHECK_EQ(items[0].label, std::string("printf"));
  CHECK_EQ(items[1].insert, std::string("print("));
  m = parse("{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":[{\"uri\":\"file:///x/y.h\",\"range\":{\"start\":{\"line\":10,"
            "\"character\":2},\"end\":{\"line\":10,\"character\":5}}}]}");
  auto locs = parse_locations(m.result);
  CHECK_EQ(locs.size(), (size_t)1);
  CHECK_EQ(locs[0].path, std::string("/x/y.h"));
  CHECK_EQ(locs[0].line, 10);
  CHECK(parse_locations(fld::Json()).empty());
  CHECK(parse("not json").kind == Message::Other);
  // formatting and rename
  {
    std::string f = lsp::formatting(7, "file:///a.c", 4, true);
    CHECK(f.find("\"textDocument/formatting\"") != std::string::npos && f.find("\"tabSize\":4") != std::string::npos);
    std::string r = lsp::rename(8, "file:///a.c", 1, 2, "nn");
    CHECK(r.find("\"newName\":\"nn\"") != std::string::npos);
    std::string text = "int a;\nint  b = a;\n";
    fld::Json edits;
    CHECK(fld::Json::parse(R"([{"range":{"start":{"line":1,"character":3},"end":{"line":1,"character":5}},"newText":" "},
                              {"range":{"start":{"line":0,"character":4},"end":{"line":0,"character":5}},"newText":"x"}])",
                           edits));
    auto te = lsp::parse_text_edits(edits);
    CHECK_EQ(te.size(), (size_t)2);
    CHECK(lsp::apply_edits(text, te));
    CHECK_EQ(text, std::string("int x;\nint b = a;\n"));
    // overlapping edits are refused
    std::string t2 = "abcdef";
    std::vector<lsp::TextEdit> ov = {{0, 1, 0, 4, "X"}, {0, 2, 0, 5, "Y"}};
    CHECK(!lsp::apply_edits(t2, ov));
    // UTF-16 columns: é is one column
    std::string t3 = "é = 1;";
    std::vector<lsp::TextEdit> u = {{0, 4, 0, 5, "2"}};
    CHECK(lsp::apply_edits(t3, u));
    CHECK_EQ(t3, std::string("é = 2;"));
    fld::Json we;
    CHECK(fld::Json::parse(R"({"changes":{"file:///p/a.c":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"b"}]}})", we));
    auto files = lsp::parse_workspace_edit(we);
    CHECK_EQ(files.size(), (size_t)1);
    CHECK_EQ(files[0].first, std::string("/p/a.c"));
    CHECK(fld::Json::parse(R"({"documentChanges":[{"textDocument":{"uri":"file:///p/b.c","version":3},"edits":[]}]})", we));
    files = lsp::parse_workspace_edit(we);
    CHECK_EQ(files.size(), (size_t)1);
    CHECK_EQ(files[0].first, std::string("/p/b.c"));
  }
  return test_result("lsp");
}
