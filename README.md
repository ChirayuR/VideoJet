# VideoJet

Fast software H.264 decoding for the ESP32-S3, as an ESP-IDF component.

One Annex-B access unit in, one decoded I420 picture out. Constrained baseline (I and P slices), 800×480 and below.
On an ESP32-S3 it decodes a panning 800×480 map at **37 fps** and a real Android Auto capture at **61 fps**, about 2–3× faster than
Espressif's `esp_h264` software decoder on the same board.

The decoder core is [h264bsd](https://github.com/oneam/h264bsd) (Android's Hantro baseline decoder), rebuilt around the S3:
code and tables in internal RAM, the S3's PIE SIMD for motion compensation and the inverse transform, the deblock filter and
the bitstream parser split across both cores, and a rewritten CAVLC residual path. Every optimisation is held to one rule: the
output of every frame is bit-exact against a reference decode.

## Results

Decode only, on an ESP32-S3 (240 MHz, 8 MB octal PSRAM at 80 MHz). 800×480 streams, 300 frames each, except the capture (663).

| Stream | What it is | VideoJet | esp_h264 (dual task) | Speedup | VideoJet ms/frame |
|---|---|---|---|---|---|
| aux_card | near-static turn card | 155.19 fps | 27.94 fps | 5.55× | 6.4 |
| aux_map | panning map, top-left 640×172, black margins | 36.98 fps | 15.77 fps | 2.34× | 27.0 |
| full_map | the whole 800×480 frame changes (worst case) | 17.60 fps | 9.13 fps | 1.93× | 56.8 |
| aa_cluster_heavy | real Android Auto capture | 60.72 fps | 20.35 fps | 2.98× | 16.5 |

For scale, the unmodified h264bsd ran the first three streams at 17.12 / 9.69 / 5.60 fps.

Read these with the following in mind:
- `aux_map` is the workload the project was built for: an Android Auto second screen for a 640×172 bar. A 30 fps consumer has
  a 33.3 ms budget per frame.
- The first three streams are x264 low-QP constant-bitrate streams made to look like Android Auto. On them the deblock filter
  does nothing on most macroblocks, which flatters some of the gains. The capture is the more honest figure. It is a private
  recording and is not in this repo.
- One board, one `esp_h264` version (1.4.1). fps shifts by about 0.5 % between boots with the PSRAM timing the chip picks, so
  compare within one speed class. These are the slow class (tuning index 5).
- `esp_h264` crashed the board on the 4-slice `aux_map_dark` stream with its dual task on. That may be our harness and not a bug
  in their decoder.
- Decode only. Colour conversion and display are not included.

Raw records: [`docs/baseline-m3.jsonl`](docs/baseline-m3.jsonl) and [`docs/compare-alt.jsonl`](docs/compare-alt.jsonl).
Method and history: [`docs/bench-log.md`](docs/bench-log.md).

## Use it

Requires ESP-IDF 5.4 or newer.

```c
#include "videojet.h"

vj_decoder_t *dec = vj_decoder_create();
vj_frame_t f;
vj_status_t st = vj_decode_au(dec, au, au_len, &f);   /* one Annex-B access unit */
if (st == VJ_FRAME) {
    /* f.y, f.u, f.v: the I420 picture, f.y_stride / f.uv_stride, f.crop_w x f.crop_h */
}
vj_decoder_destroy(dec);
```

`vj_au_length()` splits a byte stream into access units. Add the component with
`set(EXTRA_COMPONENT_DIRS /path/to/VideoJet/components)` before `project()`, then `PRIV_REQUIRES videojet`.

The full guide is [`components/videojet/README.md`](components/videojet/README.md): a complete usage example, RAM requirements
(about 96–100 KB of internal RAM, plus about 1.5 MB of PSRAM at 800×480), the recommended sdkconfig, reading a frame while the
next one decodes, the error contract, and every Kconfig option.

## What it is not

- Not a full H.264 decoder: constrained baseline only (no B-frames, no CABAC, no interlace).
- Not a display driver. Colour conversion, cropping policy, rotation and the panel are the consumer's job.
- ESP32-S3 only. The code runs elsewhere but is not tuned for it.

## Reproduce the numbers

You need an ESP32-S3 board with octal PSRAM and ESP-IDF 5.4.2.

```
tools/flash_streams.sh                                  # only when the streams change
rm -rf bench/build && idf.py -C bench -B "$PWD/bench/build" -DSDKCONFIG="$PWD/bench/build/sdkconfig" build
(cd bench/build && python -m esptool --chip esp32s3 -p $PORT -b 921600 write_flash @flash_args)
python tools/bench.py --label mine --set search          # add --tuning 5 to pin the slow speed class
```

`bench.py` refuses to score unless the firmware on the board is the build you made, and every row must pass its frame hashes.
To compare against `esp_h264` and TinyH264, run `tools/fetch_alt.sh`, then build with `VJ_BENCH_DEC=esp_h264` (or `tinyh264`).
Host-side checks (hash gate, ASan, TSan, a lockstep test of the frame-hold API) run with `make -C host check`.
`CLAUDE.md` has the toolchain traps and the full bench workflow.

## Layout

| Path | What |
|---|---|
| `components/videojet/` | the component: public header, private vendored decoder (`h264bsd/`), PIE kernels |
| `bench/` | ESP-IDF app that decodes the streams on the board and prints timing and hash rows |
| `host/` | host build of the decoder, unit tests, sanitizer and hash checks |
| `tools/` | the scorer (`bench.py`), comparison, golden-hash and stream tools |
| `streams/` | test streams and their golden per-frame hashes |
| `docs/` | design spec, plans, bench log with every measurement |

## Licence

Apache-2.0, see [`LICENSE`](LICENSE). The vendored h264bsd keeps its own notice in `components/videojet/h264bsd/LICENSE.md`.
