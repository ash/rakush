#!/bin/sh
# Feed t/session.rksh to rakush and compare what it prints with t/session.out.
here=$(cd "$(dirname "$0")/.." && pwd)
bin=${1:-$here/rakush}
out=$(mktemp)
RAKUSH_TEST_DIR=$here RAKUSH_TEST_VAR='x  y' RAKUSH_TEST_GLOB='t/*.out' "$bin" < "$here/t/session.rksh" > "$out" 2>&1
status=0
if diff -u "$here/t/session.out" "$out"; then
    echo "ok - session"
else
    echo "not ok - session"
    status=1
fi
rm -f "$out"

got=$("$bin" -c "printf 'a\nb\n' | { .uc } | head -1" 2>&1; echo "status $?"
      "$bin" -c "ls /nonexistent-rakush-test" 2>/dev/null; echo "status $?")
want=$(printf 'A\nstatus 0\nstatus 1')
if [ "$got" = "$want" ]; then
    echo "ok - rakush -c"
else
    echo "not ok - rakush -c: got [$got]"
    status=1
fi
exit $status
