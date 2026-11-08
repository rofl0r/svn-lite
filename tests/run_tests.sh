#!/bin/sh
set -u
cd "$(dirname "$0")/.."
PASS=0; FAIL=0; MOCK_PID=""; TMP=$(mktemp -d)
trap 'kill $MOCK_PID 2>/dev/null; rm -rf "$TMP"' EXIT INT TERM
start_mock() {
    PORT=0; : >"$TMP/mock.log"
    "$@" nohup python3 tests/mock_svn.py 0 >"$TMP/mock.log" 2>&1 &
    MOCK_PID=$!; i=0; PORT=0
    while [ $i -lt 50 ]; do
        if [ -s "$TMP/mock.log" ]; then PORT=$(head -1 "$TMP/mock.log"); case "$PORT" in ''|*[!0-9]*) ;; *) return 0;; esac; fi
        sleep 0.1; i=$((i+1))
    done
    echo "FAIL: mock did not start"; exit 1
}
stop_mock() { if [ -n "$MOCK_PID" ]; then kill "$MOCK_PID" 2>/dev/null; wait "$MOCK_PID" 2>/dev/null; MOCK_PID=""; fi; sleep 1; }
ok() { echo "ok - $1"; PASS=$((PASS+1)); }
bad() { echo "FAIL - $1"; FAIL=$((FAIL+1)); }
make >/dev/null 2>&1 || { echo "FAIL: build"; exit 1; }
start_mock env
URL="svn://127.0.0.1:$PORT/mock"
./svn info "$URL" >"$TMP/info" 2>&1; if grep -q "Revision: 42" "$TMP/info"; then ok "info revision"; else bad "info revision"; cat "$TMP/info"; fi
if grep -q "Last Changed Author: tester" "$TMP/info"; then ok "info author"; else bad "info author"; fi
if grep -q "2020-11-10 09:23:51" "$TMP/info"; then ok "info date"; else bad "info date"; fi
./svn log "$URL" >"$TMP/log" 2>&1
if grep -q "log message with ( unbalanced parens" "$TMP/log"; then ok "log message"; else bad "log message"; cat "$TMP/log"; fi
if test "$(grep -c 'log message with' "$TMP/log")" = 3; then ok "log not truncated"; else bad "log not truncated"; fi
timeout 20 ./svn checkout "$URL" "$TMP/co" >"$TMP/co.log" 2>&1; rc=$?
if test $rc = 0; then ok "checkout succeeds"; else bad "checkout succeeds"; cat "$TMP/co.log"; fi
if test -f "$TMP/co/small.txt" && test -f "$TMP/co/space file.txt" && test -f "$TMP/co/colon:name.txt" && test -f "$TMP/co/paren(1).txt" && test -f "$TMP/co/paren(unbalanced.txt"; then ok "special names"; else bad "special names"; find "$TMP/co" -type f; fi
if test -f "$TMP/co/sub dir/inner (x).txt"; then ok "subdir with space"; else bad "subdir with space"; fi
if test "$(wc -c < "$TMP/co/exact4096.bin")" = 4096; then ok "4096 file"; else bad "4096 file"; fi
if test "$(wc -c < "$TMP/co/exact8192.bin")" = 8192; then ok "8192 file"; else bad "8192 file"; fi
if test "$(wc -c < "$TMP/co/big.bin")" = 100000; then ok "100000 file"; else bad "100000 file"; fi
if test ! -s "$TMP/co/empty.txt"; then ok "empty file"; else bad "empty file"; fi
python3 - "$TMP/co" <<'PY'
import hashlib, os, sys
root=sys.argv[1]; bad=[]
for line in open(os.path.join(root,".svnup","known_files")):
    md5,path=line.rstrip("\n").split("\t")
    if hashlib.md5(open(root+path,"rb").read()).hexdigest()!=md5: bad.append(path)
sys.exit(1 if bad else 0)
PY
if test $? = 0; then ok "md5 matches"; else bad "md5 matches"; fi
timeout 20 ./svn checkout "$URL" "$TMP/co" >/dev/null 2>&1; if test $? = 0; then ok "second checkout"; else bad "second checkout"; fi
if timeout 15 ./svn info "$URL/small.txt" >/dev/null 2>&1; then bad "file info rejected"; else ok "file info rejected"; fi
stop_mock
start_mock env MOCK_DATE="2020-11-10T09:23:51Z"
URL="svn://127.0.0.1:$PORT/mock"
timeout 15 ./svn info "$URL" >"$TMP/date" 2>&1; rc=$?
if test $rc = 0 && grep -q "2020-11-10 09:23:51" "$TMP/date"; then ok "date without fraction"; else bad "date without fraction"; cat "$TMP/date"; fi
stop_mock
echo "pass: $PASS fail: $FAIL"
test "$FAIL" = 0
