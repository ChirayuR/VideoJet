#!/bin/bash
# usage: tools/score.sh LABEL [BASELINE]   exit 0 KEEP, 1 DROP (correct, not faster), 2 FAIL
# Host gate -> fresh IDF build -> flash @flash_args -> bench (search set) -> compare --gain 0.3.
set -uo pipefail
LABEL=$1; BASE=${2:-docs/baseline-m3.jsonl}
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
IDF_PY=${IDF_PY:-$HOME/.espressif/python_env/idf5.4_py3.10_env/bin/python}
command -v idf.py >/dev/null || source "$HOME/esp/esp-idf-v5.4.2/export.sh" >/dev/null 2>&1
B="$ROOT/bench/build-score"
timeout 1200 make -C host check >/dev/null || { echo "FAIL host gate"; exit 2; }
rm -rf "$B"
idf.py -C bench -B "$B" -DSDKCONFIG="$B/sdkconfig" build 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -2
[ -f "$B/videojet_bench.elf" ] || { echo "FAIL build"; exit 2; }
(cd "$B" && "$IDF_PY" -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash @flash_args) \
  2>&1 | tr '\r' '\n' | grep -v '%)' | tail -2 || { echo "FAIL flash"; exit 2; }
"$IDF_PY" tools/bench.py --label "$LABEL" --set search --elf "$B/videojet_bench.elf" || { echo "FAIL bench"; exit 2; }
python3 tools/compare.py "$LABEL" --baseline "$BASE" --gain 0.3 && { echo KEEP; exit 0; }
echo DROP; exit 1
