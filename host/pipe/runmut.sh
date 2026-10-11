#!/usr/bin/env bash
# usage: runmut.sh NAME...  -- each mutant must show DIFF lines against the base on clean + damaged runs
SP=$SCRATCH
cd $SP/pipe-h
ALL="$SP/wt-pipe/streams/*/*.h264 xs/*.h264 nc/*.h264 $SP/wt-pipe/host/vectors/*.h264"
for m in "$@"; do
  r=$(./runall.sh base/build-fz/fz mut-$m/build/fz 3 "1000000000 6" 1 $ALL | awk "/DIFF/{d++} / OK/{o=\$1} END{print \"diffs=\" d+0 \" ok=\" o+0}")
  echo "$m: $r"
done
