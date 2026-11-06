#!/usr/bin/env python3
"""Minimal scripted language server used by the kajo-ide LSP tests.

It speaks real LSP framing over stdio and implements just enough behaviour to
exercise the client and manager: diagnostics on didOpen/didChange, hover,
completion (with textEdit), definition, rename (WorkspaceEdit), formatting and
code actions, plus a server->client request and a showMessage notification.
"""
import json
import re
import sys

docs = {}          # uri -> text
log = open("/dev/null", "w")


def read_message():
    headers = {}
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.decode().strip()
        if line == "":
            break
        k, _, v = line.partition(":")
        headers[k.lower()] = v.strip()
    length = int(headers.get("content-length", "0"))
    body = sys.stdin.buffer.read(length)
    return json.loads(body)


def send(obj):
    data = json.dumps(obj).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(data) + data)
    sys.stdout.buffer.flush()


def respond(msg, result):
    send({"jsonrpc": "2.0", "id": msg["id"], "result": result})


def respond_error(msg, text):
    send({"jsonrpc": "2.0", "id": msg["id"], "error": {"code": -32603, "message": text}})


def word_at(text, line, char):
    lines = text.split("\n")
    if line >= len(lines):
        return None
    s = lines[line]
    # LSP characters are UTF-16 units; the tests only use BMP text.
    i = min(char, len(s))
    a = i
    while a > 0 and (s[a - 1].isalnum() or s[a - 1] == "_"):
        a -= 1
    b = i
    while b < len(s) and (s[b].isalnum() or s[b] == "_"):
        b += 1
    return s[a:b] if b > a else None


def diagnostics_for(uri):
    text = docs.get(uri, "")
    diags = []
    for n, ln in enumerate(text.split("\n")):
        idx = ln.find("BUG")
        if idx >= 0:
            diags.append({
                "range": {"start": {"line": n, "character": idx},
                          "end": {"line": n, "character": idx + 3}},
                "severity": 1, "source": "mock", "message": "found a BUG marker",
            })
        idx = ln.find("TODO")
        if idx >= 0:
            diags.append({
                "range": {"start": {"line": n, "character": idx},
                          "end": {"line": n, "character": idx + 4}},
                "severity": 2, "source": "mock", "message": "unfinished TODO",
            })
    return diags


def publish(uri):
    send({"jsonrpc": "2.0", "method": "textDocument/publishDiagnostics",
          "params": {"uri": uri, "diagnostics": diagnostics_for(uri)}})


def handle(msg):
    method = msg.get("method")
    params = msg.get("params", {})
    if method == "initialize":
        respond(msg, {"capabilities": {
            "textDocumentSync": 1,
            "hoverProvider": True,
            "completionProvider": {"triggerCharacters": ["."]},
            "definitionProvider": True,
            "implementationProvider": True,
            "renameProvider": True,
            "documentFormattingProvider": True,
            "codeActionProvider": {"resolveProvider": False},
        }, "serverInfo": {"name": "mock"}})
    elif method == "initialized":
        # exercise the server->client request path
        send({"jsonrpc": "2.0", "id": 9001, "method": "workspace/configuration",
              "params": {"items": [{"section": "a"}, {"section": "b"}]}})
        send({"jsonrpc": "2.0", "method": "window/showMessage",
              "params": {"type": 1, "message": "mock ready"}})
    elif method == "textDocument/didOpen":
        td = params["textDocument"]
        docs[td["uri"]] = td["text"]
        publish(td["uri"])
    elif method == "textDocument/didChange":
        uri = params["textDocument"]["uri"]
        docs[uri] = params["contentChanges"][-1]["text"]
        publish(uri)
    elif method == "textDocument/didClose":
        docs.pop(params["textDocument"]["uri"], None)
    elif method == "textDocument/hover":
        pos = params["position"]
        w = word_at(docs.get(params["textDocument"]["uri"], ""), pos["line"], pos["character"])
        if w is None:
            respond(msg, None)
        else:
            respond(msg, {"contents": {"kind": "markdown",
                                        "value": "```go\nfunc %s(x int) string\n```\n\n**%s** does a thing. See [docs](http://x)." % (w, w)}})
    elif method == "textDocument/completion":
        uri = params["textDocument"]["uri"]
        pos = params["position"]
        line = docs.get(uri, "").split("\n")[pos["line"]]
        a = pos["character"]
        while a > 0 and (line[a - 1].isalnum() or line[a - 1] == "_"):
            a -= 1
        rng = {"start": {"line": pos["line"], "character": a},
               "end": {"line": pos["line"], "character": pos["character"]}}
        items = [
            {"label": "Println", "kind": 2, "detail": "func(a ...any)",
             "sortText": "0001", "textEdit": {"range": rng, "newText": "Println"},
             "documentation": {"kind": "markdown", "value": "Println formats."}},
            {"label": "Printf", "kind": 2, "detail": "func(format string)", "sortText": "0002",
             "insertText": "Printf(${1:format})", "insertTextFormat": 2,
             "additionalTextEdits": [{"range": {"start": {"line": 0, "character": 0},
                                                 "end": {"line": 0, "character": 0}},
                                       "newText": "import \"fmt\"\n"}]},
            {"label": "Sprint", "kind": 2, "detail": "func", "sortText": "0003"},
        ]
        respond(msg, {"isIncomplete": False, "items": items})
    elif method in ("textDocument/definition", "textDocument/implementation"):
        uri = params["textDocument"]["uri"]
        respond(msg, [{"uri": uri, "range": {"start": {"line": 2, "character": 4},
                                              "end": {"line": 2, "character": 7}}}])
    elif method == "textDocument/rename":
        uri = params["textDocument"]["uri"]
        pos = params["position"]
        text = docs.get(uri, "")
        w = word_at(text, pos["line"], pos["character"])
        if not w:
            respond_error(msg, "nothing to rename")
            return
        edits = []
        for n, ln in enumerate(text.split("\n")):
            for m in re.finditer(r"\b%s\b" % re.escape(w), ln):
                edits.append({"range": {"start": {"line": n, "character": m.start()},
                                         "end": {"line": n, "character": m.end()}},
                              "newText": params["newName"]})
        respond(msg, {"changes": {uri: edits}})
    elif method == "textDocument/formatting":
        uri = params["textDocument"]["uri"]
        text = docs.get(uri, "")
        edits = []
        for n, ln in enumerate(text.split("\n")):
            stripped = ln.rstrip()
            if stripped != ln:
                edits.append({"range": {"start": {"line": n, "character": len(stripped)},
                                         "end": {"line": n, "character": len(ln)}},
                              "newText": ""})
        respond(msg, edits)
    elif method == "textDocument/codeAction":
        uri = params["textDocument"]["uri"]
        n_diags = len(params.get("context", {}).get("diagnostics", []))
        line = params["range"]["start"]["line"]
        respond(msg, [{
            "title": "Fix BUG (%d diag)" % n_diags, "kind": "quickfix", "isPreferred": True,
            "edit": {"changes": {uri: [{"range": {"start": {"line": line, "character": 0},
                                                    "end": {"line": line, "character": 0}},
                                          "newText": "// fixed\n"}]}},
        }])
    elif method == "shutdown":
        respond(msg, None)
    elif method == "exit":
        sys.exit(0)
    elif "id" in msg and method is not None:
        respond_error(msg, "method not found: %s" % method)


def main():
    while True:
        msg = read_message()
        if msg is None:
            break
        # responses to our server->client requests carry no method
        if "method" not in msg:
            continue
        handle(msg)


if __name__ == "__main__":
    main()
