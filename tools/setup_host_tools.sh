#!/usr/bin/env bash
# Host tools for VideoJet: a bundled ffmpeg (no sudo needed) for golden decodes, plus x264,
# and the numpy/Pillow that tools/gen_streams.py needs (Pillow >= 10.1 for load_default(size=)).
set -euo pipefail
python3 -m pip install --user --quiet imageio-ffmpeg
python3 -m pip install --user --quiet "numpy>=1.26" "pillow>=10.1"
ff=$(python3 -c 'import imageio_ffmpeg; print(imageio_ffmpeg.get_ffmpeg_exe())')
"$ff" -hide_banner -version | head -1
x264 --version | head -1
python3 -c 'import numpy; print("numpy", numpy.__version__)'
python3 -c 'import PIL; print("pillow", PIL.__version__)'
