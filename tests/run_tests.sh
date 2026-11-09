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
if test "$(grep -c 'log message with' "$TMP/log")" = 2; then ok "log not truncated"; else bad "log not truncated"; fi
if grep -q "after" "$TMP/log"; then ok "log with NUL is kept"; else bad "log with NUL is kept"; cat "$TMP/log"; fi
timeout 20 ./svn checkout "$URL" "$TMP/co" >"$TMP/co.log" 2>&1; rc=$?
./svn log "$TMP/co" >"$TMP/localinfo" 2>&1; if grep -q "after" "$TMP/localinfo"; then ok "local log survives NUL roundtrip"; else bad "local log survives NUL roundtrip"; cat "$TMP/localinfo"; fi
if test $rc = 0; then ok "checkout succeeds"; else bad "checkout succeeds"; cat "$TMP/co.log"; fi
if test -f "$TMP/co/small.txt" && test -f "$TMP/co/space file.txt" && test -f "$TMP/co/colon:name.txt" && test -f "$TMP/co/paren(1).txt" && test -f "$TMP/co/paren(unbalanced.txt"; then ok "special names"; else bad "special names"; find "$TMP/co" -type f; fi
if test -f "$TMP/co/sub dir/inner (x).txt"; then ok "subdir with space"; else bad "subdir with space"; fi
if test -f "$TMP/co/deep/a/b/c/deep file.txt"; then ok "deep nested path"; else bad "deep nested path"; fi
if test "$(wc -c < "$TMP/co/exact4096.bin")" = 4096; then ok "4096 file"; else bad "4096 file"; fi
if test "$(wc -c < "$TMP/co/exact8192.bin")" = 8192; then ok "8192 file"; else bad "8192 file"; fi
if test "$(wc -c < "$TMP/co/big.bin")" = 100000; then ok "100000 file"; else bad "100000 file"; fi
if test -x "$TMP/co/exec.sh"; then ok "executable prop"; else bad "executable prop"; fi
if test -L "$TMP/co/link.txt"; then ok "special prop symlink"; else bad "special prop symlink"; fi
if test ! -x "$TMP/co/small.txt"; then ok "decoy prop value not executable"; else bad "decoy prop value not executable"; fi
if test ! -s "$TMP/co/empty.txt"; then ok "empty file"; else bad "empty file"; fi
python3 - "$TMP/co" <<'PY'
import hashlib, os, sys
root=sys.argv[1]; bad=[]
for line in open(os.path.join(root,".svnup","known_files")):
    md5,path=line.rstrip("\n").split("\t")
    data = b"link small.txt" if os.path.islink(root+path) else open(root+path,"rb").read()
    if hashlib.md5(data).hexdigest()!=md5: bad.append(path)
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
start_mock env MOCK_CHUNK=100
URL="svn://127.0.0.1:$PORT/mock"
timeout 20 ./svn checkout "$URL" "$TMP/co100" >"$TMP/co100.log" 2>&1; rc=$?
if test $rc = 0 && test "$(wc -c < "$TMP/co100/big.bin")" = 100000; then ok "100 byte chunk checkout"; else bad "100 byte chunk checkout"; cat "$TMP/co100.log"; fi
stop_mock
start_mock env MOCK_FAIL_FIRST=1
URL="svn://127.0.0.1:$PORT/mock"
if timeout 15 ./svn checkout "$URL" "$TMP/cofail" >"$TMP/cofail.log" 2>&1; then bad "dropped connection fails cleanly"; else ok "dropped connection fails cleanly"; fi
stop_mock
start_mock env
URL="svn://127.0.0.1:$PORT/mock"
timeout 20 ./svn checkout "$URL" "$TMP/cofail2" >"$TMP/cofail2.log" 2>&1; rc=$?
if test $rc = 0 && test -f "$TMP/cofail2/small.txt"; then ok "clean checkout after dropped connection"; else bad "clean checkout after dropped connection"; cat "$TMP/cofail2.log"; fi
stop_mock
start_mock env MOCK_SPLIT=1
URL="svn://127.0.0.1:$PORT/mock"
timeout 30 ./svn checkout "$URL" "$TMP/cosplit" >"$TMP/cosplit.log" 2>&1; rc=$?
if test $rc = 0 && test "$(cat "$TMP/cosplit/small.txt")" = "hello (world) : keep"; then ok "1 byte split checkout"; else bad "1 byte split checkout"; cat "$TMP/cosplit.log"; fi
timeout 15 ./svn log "$URL" >"$TMP/splitlog.log" 2>&1; if grep -q "log message with" "$TMP/splitlog.log"; then ok "1 byte split log"; else bad "1 byte split log"; cat "$TMP/splitlog.log"; fi
stop_mock
echo "pass: $PASS fail: $FAIL"
test "$FAIL" = 0
