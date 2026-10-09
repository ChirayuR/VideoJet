#!/usr/bin/env bash
# Write build-host/streams.bin into the `streams` partition (0x310000, partitions.csv).
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
. "$HOME/esp/esp-idf-v5.4.2/export.sh" > /dev/null
esptool.py --chip esp32s3 -p "${PORT:-/dev/ttyACM0}" -b 921600 write_flash 0x310000 "$root/build-host/streams.bin"
