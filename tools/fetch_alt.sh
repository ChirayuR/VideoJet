#!/usr/bin/env bash
# Fetch the decoders the bench compares VideoJet with into bench/alt/ (gitignored: third-party code, never committed).
#   esp_h264 1.4.1: Espressif's component. Its software decoder is a prebuilt tinyH264 (custom licence, no source).
#   tinyH264 524a702: github.com/udevbe/tinyh264, h264bsd trimmed for WebAssembly (Apache-2.0). native/ only.
# A comparison build names one in the environment; the rest is the board loop in CLAUDE.md:
#   VJ_BENCH_DEC=esp_h264 idf.py -C bench -B "$PWD/bench/build-esp_h264" -DSDKCONFIG="$PWD/bench/build-esp_h264/sdkconfig" build
# esp_h264's dual task is CONFIG_ESP_H264_DUAL_TASK=y, in an extra -DSDKCONFIG_DEFAULTS file.
set -euo pipefail
cd "$(dirname "$0")/../bench/alt"
if [ ! -f esp_h264/CMakeLists.txt ]; then
    curl -fsSL -o esp_h264.zip \
        https://components-file.espressif.com/components/espressif/esp_h264/1.4.1/espressif__esp_h264-v1.4.1.zip
    unzip -q esp_h264.zip -d esp_h264
    rm esp_h264.zip
fi
if [ ! -d tinyh264/native ]; then
    git clone -q https://github.com/udevbe/tinyh264 tinyh264/repo
    git -C tinyh264/repo checkout -q 524a702
    mv tinyh264/repo/LICENSE tinyh264/repo/native/
    mv tinyh264/repo/native tinyh264/
    rm -rf tinyh264/repo
fi
