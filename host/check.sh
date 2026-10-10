#!/usr/bin/env bash
# usage: check.sh VJDEC STREAMS_DIR
# Every X.golden under STREAMS_DIR must equal VJDEC's output for X.h264, and VJDEC must exit 0.
# Every X.h264 must have an X.golden: a stream without one verifies nothing, so it is a FAIL, except under a
# failing/ directory (where a stream that diverges is parked, its .golden renamed so this gate skips it).
# Fails on a missing or empty stream/golden, an orphan stream, and when there is no golden at all (nothing verified).
# On FAIL it shows vjdec's stderr and cmp's first difference (the line number is the first bad frame + 1).
set -u
dec=${1:?usage: check.sh VJDEC STREAMS_DIR}
dir=${2:?usage: check.sh VJDEC STREAMS_DIR}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
n=0
fail=0
while IFS= read -r g; do
    s=${g%.golden}.h264
    n=$((n + 1))
    if [ ! -s "$s" ] || [ ! -s "$g" ]; then
        echo "FAIL $s (stream or golden missing or empty)"
        fail=1
        continue
    fi
    "$dec" "$s" </dev/null >"$tmp/out" 2>"$tmp/err"
    rc=$?
    if [ $rc -eq 0 ] && cmp -s "$tmp/out" "$g"; then
        echo "PASS $s"
    else
        echo "FAIL $s (vjdec exit $rc)"
        sed 's/^/    vjdec: /' "$tmp/err"
        cmp "$tmp/out" "$g" 2>&1 | sed 's/^/    /'
        fail=1
    fi
done < <(find "$dir" -name '*.golden' | sort)
while IFS= read -r s; do
    case ${s#"${dir%/}"/} in failing/* | */failing/*) continue ;; esac
    if [ ! -e "${s%.h264}.golden" ]; then
        n=$((n + 1))
        echo "FAIL $s (no .golden: nothing verifies it)"
        fail=1
    fi
done < <(find "$dir" -name '*.h264' | sort)
if [ $n -eq 0 ]; then
    echo "FAIL no .golden files under $dir"
    fail=1
fi
echo "$n streams checked"
exit $fail
