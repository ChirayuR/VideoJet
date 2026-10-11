#!/usr/bin/env bash
# usage: build.sh [variants...]  -- sync + build fz variants into new/build-NAME (WORKER=0 sequential unless NAME has w)
SP=$SCRATCH
$SP/pipe-h/sync.sh
cd $SP/pipe-h/new/host
for v in "$@"; do
  n=${v%%:*}; f=${v#*:}; wk=0; case $n in w*) wk=1;; esac
  make B=../build-$n WORKER=$wk CFLAGS="-std=gnu99 -O2 -g -DVJ_FZ_STORAGE $f" ../build-$n/fz 2>&1 | grep -E "error|Error"
done
