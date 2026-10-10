#!/bin/bash
# usage: tools/score.sh LABEL [BASELINE]   exit 0 KEEP, 1 DROP (correct, not faster), 2 FAIL
# Host gate -> fresh IDF build -> flash @flash_args -> bench (search set) -> compare --gain 0.3.
# A run ends with one verdict line: KEEP or DROP (printed by compare.py), or FAIL <step>.
# The host gate's output goes to results/score-LABEL-gate.log; its last 30 lines are shown when it fails.
set -uo pipefail
LABEL=$1; BASE=${2:-docs/baseline-m3.jsonl}
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
IDF_PY=${IDF_PY:-$HOME/.espressif/python_env/idf5.4_py3.10_env/bin/python}
command -v idf.py >/dev/null || source "$HOME/esp/esp-idf-v5.4.2/export.sh" >/dev/null 2>&1
B="$ROOT/bench/build-score"
mkdir -p results
GATE="results/score-$LABEL-gate.log"
timeout 1200 make -C host check >"$GATE" 2>&1 || { tail -30 "$GATE"; echo "FAIL host gate"; exit 2; }
rm -rf "$B"
idf.py -C bench -B "$B" -DSDKCONFIG="$B/sdkconfig" build 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -2
[ -f "$B/videojet_bench.elf" ] || { echo "FAIL build"; exit 2; }
(cd "$B" && "$IDF_PY" -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash @flash_args) \
  2>&1 | tr '\r' '\n' | grep -v '%)' | tail -2 || { echo "FAIL flash"; exit 2; }
"$IDF_PY" tools/bench.py --label "$LABEL" --set search --elf "$B/videojet_bench.elf" || { echo "FAIL bench"; exit 2; }
python3 tools/compare.py "$LABEL" --baseline "$BASE" --gain 0.3  # prints KEEP (exit 0) or DROP (exit 1)
rc=$?
[ $rc -le 1 ] && exit $rc
echo "FAIL compare"; exit 2
