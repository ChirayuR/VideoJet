#!/usr/bin/env bash
cd $SCRATCH/pipe-h
ALL="../wt-pipe/streams/*/*.h264 xs/*.h264 nc/*.h264 ../wt-pipe/host/vectors/*.h264"
SMALL="xs/ms4.h264 xs/ionly.h264 xs/p4x4.h264 xs/cip.h264 xs/lowbr.h264 xs/nodb.h264 xs/dbo.h264 nc/*.h264 ../wt-pipe/host/vectors/*.h264"
export P=10
B=base/build-fz/fz
echo "== clean x50 (pipe)"; ./runall.sh $B new/build-pipe/fz 1 "1000000000" 50 $ALL
echo "== damaged 100 seeds x rates 3,6 (pipe)"; ./runall.sh $B new/build-pipe/fz 100 "3 6" 1 $ALL
echo "== depth1 / depth2"; ./runall.sh $B new/build-pipe1/fz 25 "4" 1 $ALL; ./runall.sh $B new/build-pipe2/fz 25 "4" 1 $ALL
echo "== jitter 400"; ./runall.sh $B new/build-pipej/fz 25 "4 1000000000" 2 $ALL
echo "== jitter 3000 (small streams)"; ./runall.sh $B new/build-pipej3/fz 6 "4 1000000000" 1 $SMALL
echo "== poison+pipe"; ./runall.sh $B new/build-pipep/fz 25 "4" 1 $ALL
echo "== storage mode + pipe (pipe gated off)"; ./runall.sh $B new/build-pipes/fz 10 "4" 1 $ALL
echo "== starvation NOSTEP (small streams)"; ./runall.sh $B new/build-pipens/fz 3 "4 1000000000" 1 $SMALL
echo "== TSan fz (small streams)"; P=6 ULIM=0 ./runall.sh $B $PWD/tsanfz.sh 8 "4" 1 $SMALL
echo "== ASan fz (small streams)"; P=6 ULIM=0 ./runall.sh $B $PWD/new/build-asanfz/fz 8 "4" 1 $SMALL
echo DONE
