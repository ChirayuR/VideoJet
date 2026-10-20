# VideoJet

A fast H.264 decoder for the ESP32-S3. ESP-IDF component, constrained baseline, Annex-B in, I420 out.

I built it to show Android Auto's second screen on a 640×172 bar. The target is 800×480 at 30 fps. It does 37 fps on a
panning map and 61 fps on a real Android Auto capture, about 2-3× faster than Espressif's `esp_h264` on the same board.

The core is [h264bsd](https://github.com/oneam/h264bsd) (the Android baseline decoder). I moved the hot code and tables into
internal RAM, wrote PIE kernels for motion compensation and the inverse transform, split parsing and deblocking across both
cores, and rewrote CAVLC. Every change has to give bit-exact output, checked on every frame against a reference decode.

## Results

Decode only, ESP32-S3 at 240 MHz, 8 MB octal PSRAM at 80 MHz. 800×480, 300 frames per stream (663 for the capture).

| Stream | What it is | VideoJet | esp_h264 | Speedup | ms/frame |
|---|---|---|---|---|---|
| aux_card | Android Auto turn-by-turn card: mostly still, only the distance text changes | 155.19 fps | 27.94 fps | 5.55× | 6.4 |
| aux_map | A moving map in the top-left 640×172 corner of an 800×480 frame, black everywhere else. This is what the bar shows | 36.98 fps | 15.77 fps | 2.34× | 27.0 |
| full_map | The same moving map filling the whole 800×480 frame. The hardest case, every pixel changes | 17.60 fps | 9.13 fps | 1.93× | 56.8 |
| aa_cluster_heavy | A real recording of an Android Auto session with a moving map (663 frames at about 1.1 Mbit/s) | 60.72 fps | 20.35 fps | 2.98× | 16.5 |

How to read the table:
- **fps** is frames decoded per second. A phone sends 30 fps, so anything above 30 keeps up.
- **esp_h264** is Espressif's own software H.264 decoder, run in its two-core mode on the same board.
- **Speedup** is VideoJet's fps divided by esp_h264's.
- The first three streams are test videos I made with x264 to imitate what Android Auto sends for a small second screen.
  The last one is a real capture.

Plain h264bsd, before any of this, did 17.12 / 9.69 / 5.60 fps on the first three.

Things to know before you quote these:
- `aux_map` is the real workload. 30 fps is a 33.3 ms budget per frame.
- The first three streams are low-QP x264 made to look like Android Auto. Deblocking does nothing on most of their
  macroblocks, which helps VideoJet. The capture is the fairer number. It's private, so it isn't in the repo.
- One board, one `esp_h264` version (1.4.1), `esp_h264` in dual-task mode. fps moves about 0.5 % between boots with the PSRAM
  timing. These are the slow class (tuning 5).
- `esp_h264` crashed the board on a 4-slice stream (`aux_map_dark`) with dual task on. Could be my harness.
- No colour conversion or display in these numbers.

Raw records are in [`docs/baseline-m3.jsonl`](docs/baseline-m3.jsonl) and [`docs/compare-alt.jsonl`](docs/compare-alt.jsonl).
Method and history: [`docs/bench-log.md`](docs/bench-log.md).

## Use it

ESP-IDF 5.4 or newer. Add `set(EXTRA_COMPONENT_DIRS /path/to/VideoJet/components)` before `project()`, then
`PRIV_REQUIRES videojet`.

```c
#include "videojet.h"

vj_decoder_t *dec = vj_decoder_create();
vj_frame_t f;
vj_status_t st = vj_decode_au(dec, au, au_len, &f);   /* one Annex-B access unit */
if (st == VJ_FRAME) {
    /* f.y, f.u, f.v, f.y_stride, f.uv_stride, f.crop_w x f.crop_h */
}
vj_decoder_destroy(dec);
```

`vj_au_length()` splits a byte stream into access units. The full guide is in
[`components/videojet/README.md`](components/videojet/README.md): a complete example, RAM needs (about 96-100 KB internal,
1.5 MB PSRAM at 800×480), the sdkconfig to use, reading a frame while the next one decodes, errors, and every Kconfig option.

## What it isn't
- Not a full H.264 decoder. Constrained baseline only: no B-frames, no CABAC, no interlace.
- Not a display driver. Colour conversion, crop, rotation and the panel are yours.
- ESP32-S3 only.

## Reproduce the numbers

ESP32-S3 board with octal PSRAM, ESP-IDF 5.4.2.

```
tools/flash_streams.sh                                  # only when the streams change
rm -rf bench/build && idf.py -C bench -B "$PWD/bench/build" -DSDKCONFIG="$PWD/bench/build/sdkconfig" build
(cd bench/build && python -m esptool --chip esp32s3 -p $PORT -b 921600 write_flash @flash_args)
python tools/bench.py --label mine --set search          # --tuning 5 pins the slow speed class
```

`bench.py` won't score unless the firmware on the board is the build you made, and every frame hash has to match.
For `esp_h264` and TinyH264: run `tools/fetch_alt.sh`, then build with `VJ_BENCH_DEC=esp_h264` (or `tinyh264`).
Host checks (hashes, ASan, TSan, the frame-hold API) run with `make -C host check`. `CLAUDE.md` has the toolchain traps and the
full bench workflow.

## Layout

| Path | What |
|---|---|
| `components/videojet/` | the component: public header, private h264bsd (`h264bsd/`), PIE kernels |
| `bench/` | the board app: decodes the streams, prints timing and hash rows |
| `host/` | host build, unit tests, sanitizer and hash checks |
| `tools/` | scorer, comparison, golden-hash and stream tools |
| `streams/` | test streams and their golden per-frame hashes |
| `docs/` | spec, plans, and the bench log with every measurement |

## Licence

Apache-2.0, see [`LICENSE`](LICENSE). The vendored h264bsd keeps its own notice in `components/videojet/h264bsd/LICENSE.md`.
