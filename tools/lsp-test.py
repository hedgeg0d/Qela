#!/usr/bin/env python3
# Scripted JSON-RPC conversation against `qela --lsp`: opens a document,
# hovers and jumps to definitions, breaks it, checks the diagnostic, shuts
# down. Exit 0 on success.
import json
import os
import select
import subprocess
import sys
import tempfile

PROG = """struct Pt { x i64, y i64 }
fn add(a int, b int) int {
	return a + b;
}
var base int = 10;
fn main() int {
	var total int = base;
	var pt Pt = Pt{x: 1, y: 2};
	total = add(total, 5);
	total = add(total, pt.x);
	return total;
}
"""

LINES = PROG.split("\n")


def pos_of(sub):
    for i, line in enumerate(LINES):
        j = line.find(sub)
        if j >= 0:
            return i, j
    raise SystemExit(f"test bug: {sub!r} not in program")


def frame_write(f, obj):
    body = json.dumps(obj).encode()
    f.write(b"Content-Length: " + str(len(body)).encode() + b"\r\n\r\n" + body)
    f.flush()


def frame_read(f):
    hdr = b""
    while not hdr.endswith(b"\r\n\r\n"):
        c = f.read(1)
        if not c:
            return None
        hdr += c
    n = 0
    for line in hdr.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":")[1].strip())
    body = b""
    while len(body) < n:
        chunk = f.read(n - len(body))
        if not chunk:
            return None
        body += chunk
    return json.loads(body)


def recv_until(f, want, timeout=10.0):
    seen = []
    while True:
        r, _, _ = select.select([f], [], [], timeout)
        if not r:
            raise SystemExit(f"timeout waiting for {want}, got {seen}")
        msg = frame_read(f)
        if msg is None:
            raise SystemExit("server closed the pipe")
        seen.append(msg)
        if msg.get("method") == want or msg.get("id") == want:
            return msg


def main():
    compiler = os.environ.get("QELA", "build/bootstrap/s2")
    tmp = tempfile.NamedTemporaryFile(suffix=".qela", delete=False, mode="w")
    tmp.write(PROG)
    tmp.close()
    uri = "file://" + tmp.name
    os.unlink(tmp.name)

    srv = subprocess.Popen(
        [compiler, "--lsp"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL)
    r, w = srv.stdout, srv.stdin
    try:
        frame_write(w, {"jsonrpc": "2.0", "id": 1, "method": "initialize",
                        "params": {"processId": None, "rootUri": None}})
        resp = recv_until(r, 1)
        assert resp["result"]["capabilities"]["hoverProvider"] is True
        assert resp["result"]["capabilities"]["definitionProvider"] is True
        print("ok   initialize")

        frame_write(w, {"jsonrpc": "2.0", "method": "initialized", "params": {}})
        frame_write(w, {"jsonrpc": "2.0", "method": "textDocument/didOpen",
                        "params": {"textDocument": {"uri": uri, "version": 1,
                                                    "languageId": "qela", "text": PROG}}})
        diag = recv_until(r, "textDocument/publishDiagnostics")
        assert diag["params"]["uri"] == uri
        assert diag["params"]["diagnostics"] == []
        print("ok   didOpen, clean diagnostics")

        line, col = pos_of("total = add(total, 5)")
        frame_write(w, {"jsonrpc": "2.0", "id": 2, "method": "textDocument/hover",
                        "params": {"textDocument": {"uri": uri},
                                   "position": {"line": line, "character": col}}})
        resp = recv_until(r, 2)
        assert resp["result"]["contents"] == "var total i64", resp["result"]
        print("ok   hover on a local")

        line, col = pos_of("total = add(total, 5)")
        frame_write(w, {"jsonrpc": "2.0", "id": 3, "method": "textDocument/definition",
                        "params": {"textDocument": {"uri": uri},
                                   "position": {"line": line, "character": col + 12}}})
        resp = recv_until(r, 3)
        loc = resp["result"][0]
        dline, dcol = pos_of("total int = base;")
        assert loc["uri"] == uri
        assert loc["range"]["start"]["line"] == dline
        assert loc["range"]["start"]["character"] == dcol
        print("ok   definition of a local")

        line, col = pos_of("total = add(total, 5)")
        frame_write(w, {"jsonrpc": "2.0", "id": 4, "method": "textDocument/hover",
                        "params": {"textDocument": {"uri": uri},
                                   "position": {"line": line, "character": col + 8}}})
        resp = recv_until(r, 4)
        assert resp["result"]["contents"] == "fn add(i64, i64) i64", resp["result"]
        print("ok   hover on a call")

        line, col = pos_of("var total int = base;")
        frame_write(w, {"jsonrpc": "2.0", "id": 5, "method": "textDocument/definition",
                        "params": {"textDocument": {"uri": uri},
                                   "position": {"line": line, "character": col + 16}}})
        resp = recv_until(r, 5)
        loc = resp["result"][0]
        dline, dcol = pos_of("base int = 10;")
        assert loc["range"]["start"]["line"] == dline
        assert loc["range"]["start"]["character"] == dcol
        print("ok   definition of a global")

        line, col = pos_of("var total int = base;")
        frame_write(w, {"jsonrpc": "2.0", "id": 6, "method": "textDocument/completion",
                        "params": {"textDocument": {"uri": uri},
                                   "position": {"line": line, "character": col + 6}}})
        resp = recv_until(r, 6)
        labels = [i["label"] for i in resp["result"]["items"]]
        assert "total" in labels, labels
        print("ok   completion of a local")

        line, col = pos_of("pt.x")
        frame_write(w, {"jsonrpc": "2.0", "id": 7, "method": "textDocument/completion",
                        "params": {"textDocument": {"uri": uri},
                                   "position": {"line": line, "character": col + 3}}})
        resp = recv_until(r, 7)
        labels = [i["label"] for i in resp["result"]["items"]]
        assert "x" in labels and "y" in labels, labels
        print("ok   completion of fields after a dot")

        broken = PROG.replace("var total int = base;", "var total int = missing;")
        frame_write(w, {"jsonrpc": "2.0", "method": "textDocument/didChange",
                        "params": {"textDocument": {"uri": uri, "version": 2},
                                   "contentChanges": [{"text": broken}]}})
        diag = recv_until(r, "textDocument/publishDiagnostics")
        d = diag["params"]["diagnostics"]
        assert len(d) == 1, d
        assert d[0]["severity"] == 1
        dline = next(i for i, l in enumerate(broken.split("\n")) if "missing" in l)
        assert d[0]["range"]["start"]["line"] == dline
        print("ok   broken edit reports one diagnostic")

        frame_write(w, {"jsonrpc": "2.0", "id": 6, "method": "shutdown",
                        "params": None})
        resp = recv_until(r, 6)
        assert resp["result"] is None
        print("ok   shutdown")
    finally:
        srv.stdin.close()
        srv.stdout.close()
        try:
            srv.wait(timeout=10)
        except subprocess.TimeoutExpired:
            # A wedged server must not outlive the test: it would spin on
            # the leftover machine and starve the next bootstrap run.
            srv.kill()
            srv.wait(timeout=5)
            raise SystemExit("server did not exit after shutdown")
    if srv.returncode != 0:
        raise SystemExit(f"server exited {srv.returncode}")
    print("LSP test passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
