#!/usr/bin/env bash
# usage: tools/golden.sh in.h264 W H  -> writes in.golden (one vj_hash per frame of ffmpeg's decode)
set -euo pipefail
in=$1; w=$2; h=$3
root=$(cd "$(dirname "$0")/.." && pwd)
ff=$(python3 -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')
"$ff" -v error -i "$in" -fps_mode passthrough -f rawvideo -pix_fmt yuv420p - \
  | "$root/build-host/vjhash_raw" "$w" "$h" > "${in%.h264}.golden"
echo "$(wc -l < "${in%.h264}.golden") frames -> ${in%.h264}.golden"
