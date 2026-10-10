# VideoJet design (2026-10-10)

Settled context (board, research, related projects) lives in `CLAUDE.md`. This spec covers only decisions.

## What VideoJet is
- An **ESP-IDF component library** with one job: **decode H.264 (constrained baseline) as fast as possible on the ESP32-S3.**
  Its optimisations come from CubeCoders Jet.
- **Consumers** own transport, colour conversion and display.
  - The first consumer is a **separate project**: an Android Auto second-screen display on the S3.
    It receives a USB display stream and shows it on the 640×172 bar.
- Out of scope in this repo:
  - USB / the display protocol
  - YUV→RGB conversion
  - the panel
  - the P4

## Target workload and score
- Workload: the Android Auto second-screen stream (800×480, 30 fps, picture top-left 640×172, the rest blank; 440 of 1500 MBs active).
  - Every frame is a reference, so the decoder must sustain 30 fps with **≥2× headroom**.
  - It must stay correct on any constrained-baseline stream whose output order equals decode order (output reordering is off).
- Score: decoded fps on fixed streams on the board, **bit-exact** against ffmpeg.

## Architecture
```
consumer: AU bytes ─► VideoJet API ─► h264bsd fork (parse, CAVLC, MC/IDCT/intra, deblock)
                                    ─► I420 frame (PSRAM) + per-MB changed map
                      optional worker task on the other core (deblock / recon rows), chosen by the consumer
```
- `components/h264bsd`: the vendored decoder being optimised. Builds unchanged for host (x86) and ESP-IDF.
- Public API (M2), `components/videojet/include/videojet.h`, small and stable while the internals change:
  - `vj_decoder_create()` / `vj_decoder_destroy()`.
  - `vj_decode_au(dec, au, len, &frame)` takes **exactly one Annex-B access unit**. It returns `VJ_FRAME` (a complete,
    deblocked picture), `VJ_NO_FRAME` (parameter sets only) or `VJ_ERR_*`.
    - No decode work is in flight when it returns.
    - The decoder never sees the next AU, so it cannot work ahead in untimed gaps.
    - The USB protocol already frames AUs.
  - `vj_au_length(data, len)` splits a raw Annex-B buffer into AUs, for byte-stream sources (files, the head unit's TCP output).
    It handles constrained baseline without arbitrary slice order or redundant pictures.
  - A frame carries:
    - Y/U/V pointers and strides
    - coded size and crop window
    - the full-range flag and matrix coefficients
    - the IDR flag
    - concealed-MB count (so the consumer can ask the source for a keyframe)
    - `changed_mb` (NULL until M3's skip tracking lands)
  - `vj_profile_take()` returns per-stage cycle counters, or false when profiling is compiled out.
  - No config struct yet. A worker-core option arrives with M3's dual-core candidates, as an additive API.
- `bench/`: an ESP-IDF app that benchmarks the library on the board. It consumes `../components` like any app; it is not part of the library.
- `host/`: the bit-exact gate on x86.

## Correctness floor (never traded)
- Per-frame hash must equal the golden. The hash is FNV-1a-style over 32-bit words with an xorshift `h ^= h >> 15` after each multiply,
  run over the cropped Y, U and V rows; it is one shared C function in `vj_hash.h`.
  - The golden is ffmpeg's decode of the same stream, for every stream and every frame.
  - The gate (`make -C host check`) fails on zero streams, empty or orphan goldens, and decoder errors.
- A SIMD kernel must equal its C reference exactly, including alignment and tail cases.

## Eval corpus
- **Search set** (proposers see it): x264 streams made to look like AA. Constrained baseline, 1 ref, one IDR, 30 fps, 300 frames.
  - `aux_map`: a panning map in the top-left 640×172 of 800×480, with black margins.
  - `aux_card`: a turn card, mostly static, with the distance text changing.
  - `full_map`: a full-frame 800×480 pan. It is the worst case and the raw decoder-speed yardstick.
- **Held-out** (scored once at the end of each campaign):
  - h264bsd's `test_640x360.h264`
  - a dark / 4-slice / different-bitrate aux variant
  - JVT baseline conformance streams (correctness only)
  - real AUX captures from the head unit's network stream, once available

## Bench protocol (Jet's method)
- Decode-only fps.
  - Per stream: IDR µs, P-frame mean/p95/max µs, fps, hash pass/fail, minimum free internal and PSRAM heap.
  - Hashing and yields stay outside the timed region.
  - The bench must not leave decode work in flight when the timer stops (the concurrency rule for multi-core candidates, and an M2 API contract).
- Per-stage CCOUNT counters exist only in a `VJ_PROFILE` build.
- Every claimed gain is measured A/B/B/A on the board.
  - Raw runs go to `results/bench.jsonl`.
  - The summary goes to `docs/bench-log.md`.

## Optimisation ladder (the M3 meta-harness search space, decoder only)
Ordered by expected payoff:
1. **Config.** Jet's sdkconfig: 64 KB D-cache with 64 B lines, 32 KB I-cache. Document it as the recommended consumer config.
2. **Skip elision.**
   - A frame ring with a per-MB last-changed frame number. A P_Skip zero-MV MB whose target buffer is already current costs 0.
   - A deblock fast path for bS=0.
   - Export the changed-MB map.
3. **Dual core** (optional worker): row-lagged deblock on the worker core, later wavefront recon.
4. **Placement:**
   - hot code in IRAM
   - tables in DRAM
   - the MB context ring in internal SRAM
   - 64 B-aligned frame strides
5. **PIE kernels**, ordered by profile: MC copy and 6-tap, IDCT, deblock.
6. **Per profile:**
   - staging tiles for MC references
   - padded frames (no fill-block)
   - an NSAU bit reader for CAVLC

## Meta-harness framing (M3)
- **Candidate:** one mechanism change to the decoder, as a patch.
- **Scorer:** the host hash check (cheap reject), then the board bench on the search set.
- **Axes:** fps up, internal SRAM + IRAM bytes down. Consumers need internal RAM for USB and display DMA.
  - **Hard floor:** every hash passes.
- **Proposer prior:**
  - Jet techniques, one change per candidate, ponytail-minimal.
  - No stream-specific constants (resolution, crop, margins).
- **Running it:** scoring is serial on one board; proposers work in parallel.
- **Promotion:** frontier winners are re-scored A/B/B/A, then once on the held-out set.

## Memory plan (S3)
- PSRAM holds the frame ring (2–3 × 576 KB at 800×480) and cold decoder state.
- Internal RAM holds hot tables, the MB-row ring, the worker stack and IRAM code. **Library budget ≤ 100 KB internal.**
- DPB: h264bsd already sizes it to `num_ref_frames` (+1 working) when initialised with `noOutputReordering` (`h264bsd_dpb.c:1001`).
  Always init that way; the level-derived size (13 frames at L3.1) would not fit.
- Baseline bench config: IDF defaults plus `-O2`, 240 MHz, octal PSRAM 80M.

## Milestones
- **M1**:
  - Eval harness: streams, goldens, host check, board bench.
  - Baseline numbers and profile for unmodified h264bsd.
- **M2**: library packaging.
  - The public API wrapper and a component layout consumers can depend on.
  - The bench moves onto the API.
  - Fps must be unchanged.
- **M3**: meta-harness search over the ladder, until ≥60 fps decode-only on the aux streams with every hash passing.
- **Consumer project** (separate repo and spec): USB display device plus crop/convert/rotate/display on the bar, using VideoJet.
  - Jet's display techniques belong there: fused convert into DMA bands, ISR-chained DMA ring, PIE YUV→RGB565.

## Risks
- Real AUX stream properties are unknown: bitrate, slices, refs, crop origin, range. Capture early.
- x264 mode decisions differ from phone HW encoders. Treat synthetic-stream gains as provisional until real captures exist.
- PIE kernels are complex. Every kernel needs a C oracle plus an exhaustive edge test.
