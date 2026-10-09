# VideoJet design (2026-10-10)

Settled context (use case, board, panel limits, research) lives in `CLAUDE.md`. This spec covers only decisions.

## Goal
Decode Android Auto's second-screen H.264 stream on the **ESP32-S3** and show it on the 640×172 bar.
- The stream's 30 fps must hold with **≥2× headroom**.
- Score = decoded fps on fixed streams, **bit-exact** against a reference decoder.
- The pipeline takes its optimisations from CubeCoders Jet.

Out of scope here: the USB display device and head-unit software integration (sub-project 2, separate spec), and the P4.

## Architecture
```
source (flash partition now, USB later) ─► AU bytes in RAM
core 0: h264bsd fork ─ parse, CAVLC, MC/IDCT/intra ─► frame ring (PSRAM) + per-MB change map
core 1: deblock (row-lagged) ─► crop 640×172 + YUV→RGB565 + rotate + byteswap ─► internal DMA bands ─► AXS15231B QSPI
```
Boundaries, each swappable and measured on its own:
- `h264bsd` component: decoder. Upstream API plus additions (change map, frame pointers).
  It builds unchanged for the host (x86) and for ESP-IDF.
- `vj_convert`: I420 + crop rect + range/matrix (from the SPS VUI) → panel-order RGB565 band.
  A C reference version is the oracle for every SIMD variant.
- `vj_display`: panel bring-up ported from bar `display.c` (no LVGL) plus band transfer.
- `main`: bench app. Runs streams from the `streams` partition and prints one `VJ ...` result line per stream.

## Correctness floor (never traded)
- Per-frame hash (FNV-1a 32 over 32-bit words of the cropped Y, U and V rows, one shared C function) must equal the golden.
  The golden comes from the ffmpeg decode of the same stream, for every stream and every frame.
- A SIMD kernel must equal its C reference exactly, including alignment and tail cases.
- The crop origin is configurable. AUX is said to be top-left; confirm it on a real capture.

## Eval corpus
- **Search set** (proposers see it). x264 streams made to look like AA: constrained baseline, 1 ref, one IDR,
  CBR, 30 fps, 300 frames.
  - `aux_map`: a panning map in the top-left 640×172 of 800×480 with black margins.
  - `aux_card`: a turn card, mostly static, with the distance text changing.
  - `full_map`: a full-frame 800×480 pan. It is the worst case and the raw decoder-speed yardstick.
- **Held-out** (scored once, at the end of each campaign):
  - h264bsd's `test_640x360.h264`
  - a dark-theme / different-path / different-bitrate aux variant
  - real AUX captures from the head unit's network stream, once available
  - JVT baseline conformance streams (correctness only)

## Bench protocol (Jet's method)
- Decode-only fps first; the full pipeline from M2 on.
  - Per stream: IDR µs, P-frame mean/p95/max µs, fps, hash pass/fail, min free internal + PSRAM heap.
- Hashing and yields happen outside the timed region.
- Per-stage CCOUNT counters exist only in a `VJ_PROFILE` build, so timed builds stay unperturbed.
- A/B/B/A on the board for every claimed gain. Results are appended to `docs/bench-log.md`.

## Optimisation ladder (the M3 meta-harness search space)
Ordered by expected payoff:
1. Config: Jet's sdkconfig (caches 64 KB/64 B D, 32 KB I, PSRAM OCT 80M, -O2).
2. Skip elision.
   - A frame ring with a per-MB last-changed frame number. A P_Skip zero-MV MB whose target buffer is already current costs 0.
   - Deblock fast path for bS=0.
   - A frame with no change skips convert and TX.
3. Dual core: row-lagged deblock, then convert and display on core 1.
4. Placement: hot code IRAM, tables DRAM, MB context ring in SRAM, 64 B-aligned frame strides.
5. Display: a Jet ISR-chained DMA ring, if `esp_lcd` TX is found to block a core.
6. PIE kernels, ordered by profile:
   - convert (seeded from JPEGDEC `s3_simd_420.S`)
   - MC copy and 6-tap
   - IDCT
   - deblock
7. Per profile:
   - staging tiles for MC refs
   - padded frames (no fill-block)
   - an NSAU bit reader for CAVLC
   - wavefront recon on both cores

## Meta-harness framing (M3)
- Candidate: one mechanism change to the pipeline, written as a patch.
- Scorer: host hash check (cheap reject), then board bench on the search set.
- Axes: fps up; internal SRAM + IRAM bytes down. Hard floor: every hash passes.
- Proposer prior: Jet techniques, one change per candidate, ponytail-minimal.
  No stream-specific constants (resolution, crop, margins).
- Scoring is serial (one board); proposers can work in parallel.
- Frontier winners are re-scored A/B/B/A, then once on the held-out set, before promotion.

## Memory plan (S3)
- PSRAM: frame ring (2–3 × 576 KB at 800×480), the stream buffer, cold decoder state.
- Internal: 2 × 22 KB DMA bands, hot tables, the MB-row context ring, stacks. Budget ≤ 200 KB.
- DPB: h264bsd already sizes it to `num_ref_frames` (+1 working) when initialised with `noOutputReordering`
  (`h264bsd_dpb.c:1001`). Always init that way. The level-derived size (13 frames at L3.1) would not fit.
- Baseline config is IDF defaults plus `-O2`, 240 MHz and octal PSRAM 80M. Ladder step 1 adds Jet's cache and malloc settings.

## Milestones
- **M1**: eval harness (streams, goldens, host check, board bench). Baseline numbers + profile for unmodified h264bsd.
- **M2**: video on the panel (C converter); full-pipeline fps.
- **M3**: meta-harness search over the ladder, until ≥60 fps decode-only on the aux streams and 30 fps live with 0 hash failures.

## Risks
- Real AUX stream properties are unknown (bitrate, slices, refs, crop origin, range). Capture one early.
- x264 mode decisions differ from phone HW encoders, so treat synthetic-stream gains as provisional until real captures exist.
- PIE kernels are complex. Every kernel needs a C oracle plus an exhaustive edge test.
