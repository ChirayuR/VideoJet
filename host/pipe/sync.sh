#!/usr/bin/env bash
# usage: sync.sh  -- rsync the worktree into pipe-h/new and add the test hooks (kept out of the committed tree)
SP=$SCRATCH
W=$SP/wt-pipe; N=$SP/pipe-h/new
mkdir -p $N
rsync -a --delete --exclude .git --exclude 'build*' $W/components $W/host $W/streams $N/
cp -r $W/test $N/ 2>/dev/null
python3 $SP/pipe-h/mkhook.py $N
python3 $SP/pipe-h/mkpoison.py $N
