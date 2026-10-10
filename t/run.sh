#!/bin/sh
# Feed t/session.rksh to rakush and compare what it prints with t/session.out.
here=$(cd "$(dirname "$0")/.." && pwd)
bin=${1:-$here/rakush}
out=$(mktemp)
RAKUSH_TEST_DIR=$here RAKUSH_TEST_VAR='x  y' "$bin" < "$here/t/session.rksh" > "$out" 2>&1
if diff -u "$here/t/session.out" "$out"; then
    echo "ok - session"
    rm -f "$out"
else
    echo "not ok - session"
    rm -f "$out"
    exit 1
fi
