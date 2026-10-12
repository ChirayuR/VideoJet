#!/usr/bin/env bash
# usage: job.sh BASEFZ TESTFZ stream seed rate  -> "OK" or "DIFF ..."; compares stdout (status, frame hash, changed_mb hash, storage hash) and exit code
ulimit -v 4000000   # a damaged SPS can ask for GBs; both builds then fail the same way instead of an OOM kill
b=$1; t=$2; s=$3; seed=$4; rate=$5
r=$($b $s $seed $rate 2>/dev/null | md5sum | cut -c1-32; echo ${PIPESTATUS[0]})
o=$($t $s $seed $rate 2>/dev/null | md5sum | cut -c1-32; echo ${PIPESTATUS[0]})
if [ "$r" = "$o" ]; then echo OK; else echo "DIFF $s seed=$seed rate=$rate"; fi
