#!/usr/bin/env bash
# usage: fzdiff.sh FZ_WORKER FZ_SEQ STREAMS_DIR
# Differential gate for the deblock worker: for every stream, the per-call status + frame hash of fz (worker on) must equal
# that of the sequential build, undamaged and with damaged AUs (truncation, bit flips, drops, resends...), repeated REPS
# times per case (default 50) so a race shows up as a run that differs. Env: SEEDS (default 6), RATE (default 5: about
# 1 AU in 5 damaged), REPS. Also prints the changed_mb violation counts fz reports on stderr (must be 0 in both builds).
set -u
ulimit -v 4000000 # a damaged SPS can ask for GBs: both builds then fail the same way instead of an OOM kill
w=${1:?usage: fzdiff.sh FZ_WORKER FZ_SEQ STREAMS_DIR}
q=${2:?}
dir=${3:?}
SEEDS=${SEEDS:-6}
RATE=${RATE:-5}
REPS=${REPS:-50}
bad=0
n=0
while IFS= read -r s; do
    for seed in $(seq 1 "$SEEDS") clean; do
        if [ "$seed" = clean ]; then sd=1; rt=1000000000; else sd=$seed; rt=$RATE; fi
        ref=$("$q" "$s" "$sd" "$rt" 2>/tmp/fzdiff.err.$$ | md5sum | cut -c1-32)
        for i in $(seq 1 "$REPS"); do
            out=$("$w" "$s" "$sd" "$rt" 2>/tmp/fzdiff.err2.$$ | md5sum | cut -c1-32)
            n=$((n + 1))
            if [ "$out" != "$ref" ]; then
                echo "DIFF $s seed=$seed rep=$i"
                bad=$((bad + 1))
            fi
        done
        v1=$(sed -n 's/.*viol \([0-9]*\).*/\1/p' /tmp/fzdiff.err.$$)
        v2=$(sed -n 's/.*viol \([0-9]*\).*/\1/p' /tmp/fzdiff.err2.$$)
        [ "${v1:-0}" = 0 ] && [ "${v2:-0}" = 0 ] || { echo "VIOL $s seed=$seed seq=$v1 worker=$v2"; bad=$((bad + 1)); }
    done
    echo "ok   $s"
done < <(find "$dir" -name '*.h264' -not -path '*/failing/*' | sort)
rm -f /tmp/fzdiff.err.$$ /tmp/fzdiff.err2.$$
echo "$n worker runs compared, $bad differences"
[ $bad -eq 0 ]
