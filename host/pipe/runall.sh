#!/usr/bin/env bash
# usage: runall.sh BASEFZ TESTFZ SEEDS RATES REPS STREAM...  -- every stream x seed x rate (x reps), P jobs at once
b=$1; t=$2; seeds=$3; rates=$4; reps=${5:-1}; shift 5
P=${P:-16}
HERE=$(dirname "$0")
for s in "$@"; do for seed in $(seq 1 $seeds); do for rt in $rates; do for i in $(seq 1 $reps); do echo "$s $seed $rt"; done; done; done; done |
  xargs -P $P -L1 bash -c "$HERE/job.sh $b $t \$0 \$1 \$2" | sort | uniq -c
