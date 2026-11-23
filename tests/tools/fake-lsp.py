#!/usr/bin/env python3
# A tiny language server for the tests: one warning on the first line of
# every opened or changed document, two completions, and a definition at
# line 2 of the same file.
import json, sys

inp, out = sys.stdin.buffer, sys.stdout.buffer

def send(msg):
    body = json.dumps(msg).encode()
    out.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    out.flush()

def read():
    length = None
    while True:
        line = inp.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1])
    return json.loads(inp.read(length))

def diagnose(uri, text):
    first = text.split("\n")[0]
    send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
          "params": {"uri": uri, "diagnostics": [{
              "range": {"start": {"line": 0, "character": 0}, "end": {"line": 0, "character": len(first)}},
              "severity": 2, "message": "fake: %d characters" % len(first), "source": "fake"}]}})

while True:
    m = read()
    if m is None:
        break
    meth = m.get("method")
    if meth == "initialize":
        # a request from the server first, as real servers do
        send({"jsonrpc": "2.0", "id": 900, "method": "window/workDoneProgress/create", "params": {"token": "t"}})
        send({"jsonrpc": "2.0", "id": m["id"], "result": {"capabilities": {"textDocumentSync": 1}}})
    elif meth == "textDocument/didOpen":
        d = m["params"]["textDocument"]
        diagnose(d["uri"], d["text"])
    elif meth == "textDocument/didChange":
        diagnose(m["params"]["textDocument"]["uri"], m["params"]["contentChanges"][0]["text"])
    elif meth == "textDocument/completion":
        send({"jsonrpc": "2.0", "id": m["id"], "result": {"isIncomplete": False, "items": [
            {"label": "fake_one", "detail": "int"}, {"label": "fake_two", "insertText": "fake_two()"}]}})
    elif meth == "textDocument/definition":
        uri = m["params"]["textDocument"]["uri"]
        send({"jsonrpc": "2.0", "id": m["id"], "result": [
            {"uri": uri, "range": {"start": {"line": 2, "character": 4}, "end": {"line": 2, "character": 8}}}]})
    elif meth == "shutdown":
        send({"jsonrpc": "2.0", "id": m["id"], "result": None})
    elif meth == "exit":
        break
    elif "id" in m and meth:
        send({"jsonrpc": "2.0", "id": m["id"], "result": None})
