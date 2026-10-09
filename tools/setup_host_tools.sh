#!/usr/bin/env bash
# Host tools for VideoJet: a bundled ffmpeg (no sudo needed) for golden decodes, plus x264.
set -euo pipefail
python3 -m pip install --user --quiet imageio-ffmpeg
ff=$(python3 -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')
"$ff" -hide_banner -version | head -1
x264 --version | head -1
