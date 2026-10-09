#!/usr/bin/env bash
# usage: tools/golden.sh in.h264 W H  -> writes in.golden (one vj_hash per frame of ffmpeg's decode)
# Goes through a temp file: an existing golden is only replaced by a complete, non-empty result.
set -euo pipefail
in=$1; w=$2; h=$3
root=$(cd "$(dirname "$0")/.." && pwd)
ff=$(python3 -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')
out=${in%.h264}.golden
tmp=$out.tmp.$$
trap 'rm -f "$tmp"' EXIT
# format=yuv420p|yuvj420p keeps the decoder's own range: a stream with VUI full_range_flag=1 comes
# out as yuvj420p, and forcing -pix_fmt yuv420p would range-convert its samples.
if ! "$ff" -v error -i "$in" -fps_mode passthrough -vf 'format=yuv420p|yuvj420p' -f rawvideo - \
     | "$root/build-host/vjhash_raw" "$w" "$h" > "$tmp" || [ ! -s "$tmp" ]; then
    echo "golden.sh: no golden written for $in (decode failed or 0 frames)" >&2
    exit 1
fi
mv "$tmp" "$out"
echo "$(wc -l < "$out") frames -> $out"
