#!/usr/bin/env python3
"""Minimal ra_svn mock server for local tests.

Speaks just enough of the svn wire protocol (protocol version 2)
to exercise the client in ../svnup.c without a network:

  greeting -> client response -> auth request -> anonymous login
  -> repos info -> then any of: get-latest-rev, check-path,
  get-dir, get-file (props only, or with contents), log.

The in-memory repository is fixed. Paths are relative to the
URL branch the client connected with, e.g. "mock" in
svn://127.0.0.1:PORT/mock . Leading "/" in a get-file path is
stripped, matching how the client builds paths.

Configurable from the environment:
  MOCK_CHUNK   content chunk size for get-file (default 4096,
               any positive integer; 1 forces maximal splits)
  MOCK_DATE    log date string (default with fractional seconds)

Run directly:
  python3 tests/mock_svn.py [port]
"""
import hashlib
import os
import socket
import sys
import threading

CHUNK = int(os.environ.get("MOCK_CHUNK", "4096"))
DATE = os.environ.get("MOCK_DATE", "2020-11-10T09:23:51.711212Z").encode()
REV = "42"
FAIL_FIRST = os.environ.get("MOCK_FAIL_FIRST", "0") == "1"
SPLIT = int(os.environ.get("MOCK_SPLIT", "0"))
_seen_fail = False
UUID = "00000000-0000-0000-0000-000000000000"
LOG_MSG = (b"log message with ( unbalanced parens and "
           b"( 5:fake string ) and a colon: inside\n" * 2 + b"after\x00NUL\n")

FILES = ({"small.txt": b"hello (world) : keep\n"} if os.environ.get("MOCK_SMALL") else {
    "small.txt": b"hello (world) : keep\n",
    "exact4096.bin": bytes(range(256)) * 16,
    "exact8192.bin": bytes(range(256)) * 32,
    "big.bin": bytes(i % 251 for i in range(100000)),
    "bin.dat": b"\x00\xff( ) :\n" * 6000,
    "space file.txt": b"space in name\n",
    "paren(1).txt": b"paren in name\n",
    "paren(unbalanced.txt": b"unbalanced paren name\n",
    "colon:name.txt": b"colon in name\n",
    "exec.sh": b"#!/bin/sh\necho hi\n",
    "link.txt": b"link small.txt",
})
DIRS = ({} if os.environ.get("MOCK_SMALL") else {
    "sub dir": {"inner (x).txt": b"inner\n", "sub.txt": b"sub\n"},
    "plain": {"a.txt": b"a\n"},
})

def _all_files():
    out = dict(FILES)
    for d, fs in DIRS.items():
        for name, data in fs.items():
            out[d + "/" + name] = data
    return out

def S(b):
    if isinstance(b, str):
        b = b.encode()
    return str(len(b)).encode() + b":" + b + b" "

def auth_ok():
    return b"( success ( ( ) 0: ) ) "

# ---- client request parser -------------------------------------------

class Reader:
    def __init__(self, conn):
        self.c = conn
        self.buf = b""

    def _need(self, n):
        while len(self.buf) < n:
            d = self.c.recv(4096)
            if not d:
                raise EOFError
            self.buf += d

    def item(self):
        """Read one SVN item (list, string, word or number)."""
        while True:
            self._need(1)
            if self.buf[:1] not in b" \n":
                break
            self.buf = self.buf[1:]
        if self.buf[:1] == b"(":
            depth = 0
            i = 0
            while True:
                self._need(i + 1)
                ch = self.buf[i:i+1]
                if ch == b"(":
                    depth += 1
                    i += 1
                elif ch == b")":
                    depth -= 1
                    i += 1
                    if depth == 0:
                        out, self.buf = self.buf[:i], self.buf[i:]
                        return out
                elif ch.isdigit():
                    j = i
                    while True:
                        self._need(j + 1)
                        if not self.buf[j:j+1].isdigit():
                            break
                        j += 1
                    if self.buf[j:j+1] == b":":
                        n = int(self.buf[i:j])
                        self._need(j + 1 + n)
                        i = j + 1 + n
                    else:
                        i = j
                else:
                    i += 1
        else:
            i = 0
            while True:
                self._need(i + 1)
                if self.buf[i:i+1] in b" \n":
                    out, self.buf = self.buf[:i], self.buf[i:]
                    return out
                i += 1

def parse(buf, i=0):
    """Parse one item in buf, return ((type, value), next_index)."""
    while buf[i:i+1] in b" \n":
        i += 1
    if buf[i:i+1] == b"(":
        i += 1
        items = []
        while True:
            while buf[i:i+1] in b" \n":
                i += 1
            if buf[i:i+1] == b")":
                return ("list", items), i + 1
            it, i = parse(buf, i)
            items.append(it)
    if buf[i:i+1].isdigit():
        j = i
        while buf[j:j+1].isdigit():
            j += 1
        if buf[j:j+1] == b":":
            n = int(buf[i:j])
            return ("str", buf[j+1:j+1+n]), j + 1 + n
        return ("num", buf[i:j]), j
    j = i
    while j < len(buf) and buf[j:j+1] not in b" \n()":
        j += 1
    return ("word", buf[i:j]), j

# ---- server -------------------------------------------------------------

def sendall(conn, data):
    if SPLIT:
        data = bytes(data)
        for i in range(0, len(data), SPLIT):
            conn.sendall(data[i:i+SPLIT])
    else:
        conn.sendall(data)

def handle(conn):
    r = Reader(conn)
    sendall(conn, b"( success ( 2 2 ( ) ( edit-pipeline svndiff1 "
                 b"absent-entries commit-revprops depth log-revprops "
                 b"atomic-revprops partial-replay ) ) ) ")
    try:
        resp = r.item()  # client response
        url = parse(resp)[0][1][2][1].decode() if len(parse(resp)[0][1]) > 2 else ''
        sendall(conn, auth_ok())
        r.item()  # anonymous login
        sendall(conn, auth_ok())
        sendall(conn, b"( success ( " + S(UUID) + b" "
                     b"" + S("svn://127.0.0.1/mock") + b" ( ) ) ) ")
        while True:
            raw = r.item()
            (it, _) = parse(raw)
            # ( command-name params )
            cmd = it[1][0][1].decode()
            params = it[1][1][1] if len(it[1]) > 1 else []
            sendall(conn, auth_ok())
            if cmd == "get-latest-rev":
                sendall(conn, b"( success ( " + REV.encode() + b" ) ) ")
            elif cmd == "check-path":
                branch_path = url.rstrip("/").rsplit("/", 1)[-1]
                kind = b"file" if branch_path in _all_files() else b"dir"
                sendall(conn, b"( success ( " + kind + b" ) ) ")
            elif cmd == "get-dir":
                path = params[0][1].decode().lstrip("/")
                entries = []
                if path == "":
                    for name in sorted(FILES):
                        entries.append(b"( " + S(name) + b"file "
                                       + str(len(FILES[name])).encode()
                                       + b" false 0 ( " + S("1970-01-01T00:00:00.000000Z")
                                       + b") ( ) )")
                    for name in sorted(DIRS):
                        entries.append(b"( " + S(name) + b"dir 0 false 0 ( "
                                       + S("1970-01-01T00:00:00.000000Z") + b") ( ) )")
                elif path in DIRS:
                    for name in sorted(DIRS[path]):
                        entries.append(b"( " + S(name) + b"file "
                                       + str(len(DIRS[path][name])).encode()
                                       + b" false 0 ( " + S("1970-01-01T00:00:00.000000Z")
                                       + b") ( ) )")
                sendall(conn, b"( success ( " + REV.encode() + b" ( ) ( "
                             + b" ".join(entries) + b" ) ) ) ")
            elif cmd == "get-file":
                if FAIL_FIRST and not globals().get('_seen_fail'):
                    globals()['_seen_fail'] = True
                    sendall(conn, b'( success ( ( ) 0: ) ) partial')
                    conn.close()
                    return
                path = params[0][1].decode().lstrip("/")
                data = _all_files().get(path, b"")
                md5 = hashlib.md5(data).hexdigest().encode()
                want_props = params[2][1] if len(params) > 2 else b"false"
                if want_props == b"true":
                    props = b"( )"
                    if path == "exec.sh":
                        props = b"( ( " + S("svn:executable") + b" " + S("*") + b" ) )"
                    elif path == "link.txt":
                        props = b"( ( " + S("svn:special") + b" " + S("*") + b" ) )"
                    elif path == "small.txt":
                        props = b"( ( " + S("note") + b" " + S("14:svn:executable and 11:svn:special decoy") + b" ) )"
                    sendall(conn, b"( success ( ( " + S(md5) + b") "
                                 + REV.encode() + b" " + props + b" ) ) ")
                else:
                    step = CHUNK if CHUNK > 0 else 4096
                    body = b"( success ( ( " + S(md5) + b") " + REV.encode() + b" ( ) ) ) "
                    for off in range(0, len(data), step):
                        body += S(data[off:off + step])
                    body += S(b"")
                    body += b"( success ( ) ) "
                    sendall(conn, body)
                    print("sentall", len(body), flush=True)
            elif cmd == "log":
                sendall(conn, b"( ( ) " + REV.encode() + b" ( "
                             + S("tester") + b") ( " + S(DATE) + b") ( "
                             + S(LOG_MSG) + b") false false 0 ( ) false ) done ")
                sendall(conn, b"( success ( ) ) ")
            else:
                sendall(conn, b"( success ( ) ) ")
    except Exception:
        pass
    finally:
        try:
            conn.close()
        except OSError:
            pass

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(16)
    print(srv.getsockname()[1], flush=True)
    while True:
        c, _ = srv.accept()
        threading.Thread(target=handle, args=(c,), daemon=True).start()

if __name__ == "__main__":
    main()
