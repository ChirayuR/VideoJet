# VideoJet

**VideoJet is an ESP-IDF component library: fast H.264 (constrained baseline) decode on the ESP32-S3, and nothing else.**
It is a video renderer inspired by CubeCoders' Jet renderer. The goal is the most decoded frames per second, measured on
real ESP32-S3 hardware. Every optimisation must keep every output frame bit-exact against the golden hashes.

## Scope
- Target: ESP32-S3 only. The ESP32-P4 is out of scope; P4 facts below are reference only.
- Decode only. Display, USB, protocol, crop/convert/rotate code belongs to consumer projects, never in this repo.
  `bench/` and `host/` are test apparatus.
- Spec: `docs/superpowers/specs/2026-10-10-videojet-design.md`. Plans: `docs/superpowers/plans/`.
  Deferred work and the M1 decisions: `docs/superpowers/m1-carryover.md`. Scores: `docs/bench-log.md` ("M3 frontier" table).
- API: the component is `components/videojet`; the vendored decoder is private (`h264bsd/`); consumers see
  `include/videojet.h` only. `vj_decode_au(dec, au, len, &frame)` takes **one** Annex-B access unit and returns one I420
  picture; `vj_au_length()` splits a byte stream into AUs. `vj_decoder_set_hold` (opt-in) keeps a returned frame valid
  until the call after the next VJ_FRAME, for one extra reference-frame buffer (589,828 B at 800×480).
  `CONFIG_VJ_WORKER_PRIORITY` sets the worker task priority for consumers that share its core.
  Consumer guide: `components/videojet/README.md`.
- Licence: VideoJet's own code is Apache-2.0 (`LICENSE`). The decoder core is h264bsd (Android's Hantro baseline decoder,
  Apache-2.0, plain C, github.com/oneam/h264bsd).

## Working method
- Process: brainstorm → spec → plan → build. Optimisation is driven by a meta-harness; the scorer is the board bench (fps).
- Hard floor: every frame hash matches the golden. Anything that changes output is rejected.
- Compare candidates A/B/B/A across resets with warmup, and only within the same PSRAM speed class (see Baselines).
- KEEP rule for a candidate: every stream ≥ −0.3 % and at least one ≥ +0.3 %, or less internal RAM at equal fps.
  The kept candidate's score record becomes `docs/baseline-m3.jsonl`.
- Use one worktree per candidate, and review each candidate before merging.
- Serialise board access (one process at a time on the serial port), for example with `flock`.
- Keep this file current: it is the ongoing project record. Put detail in `docs/`, a summary here.

## Consumer workload (why the test streams look like this)
VideoJet's target stream comes from an Android Auto second screen: phone → head-unit server(AA dev server) → AA display
**second screen** (AUXILIARY: map or turn card, or CLUSTER) → a USB display protocol → an ESP32-S3 that decodes and drives a 640×172 bar.
- Requested stream: **H.264 baseline 800×480, 30 fps fixed**, with margins 160 right × 308 bottom. The picture is the
  top-left 640×172; the rest is blank. Consumers crop, never scale. Active area = 40×11 MBs = 440 of 1500.
- Must sustain 30 fps. The host queue is 90 AUs / 4 MiB; overflow drops to the next keyframe.
- Transport (for reference): bulk OUT = 16 B frame header (len, ts ms, flags bit0 IDR / bit1 SPS-PPS) + Annex-B AU,
  ≤16 KiB transfers. Optional IN byte 0x01 = keyframe request (≤1 per 5 s).
- AA facts: every frame is a reference (a drop corrupts until the next IDR). IDR arrives every ~10–69 s, or <3 s after a
  video-focus cycle. YUV full vs limited range varies by phone, so honour the SPS VUI flag. Margins on the MAIN display are
  centred, but the AUX picture is top-left: verify on a real capture and keep the crop origin configurable.
- Real captures: the head unit's "Network stream" output is raw H.264 on TCP :5000
  (`adb forward tcp:5000 tcp:5000`, then `nc 127.0.0.1 5000 > aux.h264`). Private captures live in the gitignored
  `streams/heldout/aa_*`.

## Reference hardware
ESP32-S3 (rev v0.2), 240 MHz, 8 MB octal PSRAM @80 MHz, 16 MB QIO flash, USB-Serial/JTAG. The bench only decodes; it never
drives a panel. Any S3 board with octal PSRAM works, but fps numbers are only comparable on the same board and PSRAM
speed class.

## Toolchain
ESP-IDF v5.4.2 (`source export.sh`). Host tools: x264 CLI, GStreamer `avdec_h264` (reference decoder), numpy, PIL. No ffmpeg
needed. Below, `$IDF_PY` is the Python of the IDF environment (it has pyserial) and `$PORT` is the board's serial port.

Traps:
- Xtensa `uint32_t` is `unsigned long`. Pass h264bsd's `u32` (`unsigned int`) to its out-params, or GCC 14 errors.
  Host builds never catch this.
- IDF's `-Werror=all` beats `-Wno-error`. Relax one tag at a time with `-Wno-error=<tag>`.
- A component's `file(GLOB ... CONFIGURE_DEPENDS ...)` is a CMake error in IDF's requirements scan (script mode). Guard it
  with `if(CMAKE_BUILD_EARLY_EXPANSION)` (plain glob) / `else()` (see `components/videojet/CMakeLists.txt`).
- esptool/idf.py progress floods output. Pipe through `tr '\r' '\n' | grep -v '%)' | tail`.
- **`idf.py -B dir flash` rebuilds from the current tree**, so you can measure the wrong tree. Flash `@flash_args` from the
  build dir with esptool instead.
- A plain `idf.py -C bench build` writes `bench/sdkconfig` (gitignored), which then silently overrides
  `sdkconfig.defaults`. Always build in a fresh dir with `-DSDKCONFIG=<dir>/sdkconfig`, and delete any `bench/sdkconfig*`
  that appears.
- Never lower the log level: the scorer identifies the firmware from the INFO-level boot line `ELF file SHA256`.

## Board bench loop
```
tools/flash_streams.sh        # only when the streams change
rm -rf bench/build && idf.py -C bench -B "$PWD/bench/build" -DSDKCONFIG="$PWD/bench/build/sdkconfig" build
(cd bench/build && $IDF_PY -m esptool --chip esp32s3 -p $PORT -b 921600 write_flash @flash_args)
$IDF_PY tools/bench.py --label X --set search
```
- `bench.py` FAILs unless the boot log's ELF SHA256 matches `bench/build/videojet_bench.elf` (`--elf PATH` for another build
  dir): flash what you built. `--set all` adds the held-out streams (about 8 min). `--tuning 5` pins the slow speed class.
- A `VJ` row ends `... fps int_min psram_min total_us int_used psram_used`: the exact timed µs and the decoder's
  internal-RAM / PSRAM heap high-water bytes, stack not counted (grammar in `tools/bench.py`).
- `tools/score.sh LABEL [BASELINE]` scores a candidate (slow class pinned).
- `python3 tools/compare.py LABEL` checks a search-set record against the M1 baseline in the same speed class (±5 %);
  `--baseline docs/baseline-m3.jsonl --tol 0.3` checks it against the M3 baseline. It cannot pass a `--set all` record,
  whose held-out streams have no baseline.
- Profile image (`CONFIG_VJ_PROFILE`): build it with the overlay so the timed build's sdkconfig never sees the switch, run
  bench with `--profile --elf bench/build-prof/videojet_bench.elf`, and reflash the timed build afterwards:
  ```
  rm -rf bench/build-prof
  idf.py -C bench -B "$PWD/bench/build-prof" -DSDKCONFIG="$PWD/bench/build-prof/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$PWD/bench/sdkconfig.defaults;$PWD/bench/sdkconfig.profile" build
  (cd bench/build-prof && $IDF_PY -m esptool --chip esp32s3 -p $PORT -b 921600 write_flash @flash_args)
  ```
- Host checks: `host/` (ASan/TSan builds, `check-asan`, `check-tsan`, `host/test_hold`). After any change to
  `vj_mc_pie.S`, re-run the on-board oracle `test/mc_pie`.
- Alternative decoders on the same bench: `VJ_BENCH_DEC=esp_h264` or `tinyh264` in the environment, after
  `tools/fetch_alt.sh` (fetched sources are gitignored).
- Gitignored, keep it that way when publishing: `backups/` (board flash dump), `results/`, `.superpowers/` (ledger),
  `streams/heldout/aa_*` (private captures).

## Performance lessons
- PIE (S3 SIMD) rules, for the library and its consumers:
  - 16 B-aligned dst; work in 8 px blocks.
  - Scalar tails: PIE silently rounds addresses down.
  - Hand-written loops in asm.
  - Exhaustive scalar-vs-SIMD tests.
- Two cores split the work; the worker runs below the caller's priority.
- Hot tables `DRAM_ATTR`, hot code `IRAM_ATTR`, cold code in flash, cold data in PSRAM. Small allocations eat internal SRAM:
  consider `heap_caps_malloc_extmem_enable()`.
- Typical sdkconfig: `-O2` (PERF), DCACHE 64 KB / 64 B line, ICACHE 32 KB, PSRAM OCT 80M,
  `SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, `RESERVE_INTERNAL=32768`. (P4: 360 MHz, PSRAM 200M, L2 256 KB / 128 B.)
- Measured: internal-SRAM frame buffer 27.9 ms vs PSRAM 33.7 ms; a PIE fill gave zero gain.
- A PSRAM line fill costs 172 cycles (0.72 µs), a dirtied line ~307. The two cores share one fill engine, so core 1's
  PSRAM traffic slows core 0. `EXTMEM_DBUS_ACS_SPIRAM_MISS_CNT` counts D-cache misses.
- D-cache autoload (`VJ_DCACHE_AUTOLOAD`) traps: a region ending at 0x3E000000 panics, and IDF flash operations clear the
  enable bit. **With WiFi DMA into PSRAM** (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) it hangs the bus about 1 s into a live stream
  (interrupt-WDT system reset, no panic text). Consumers with WiFi keep it off; the bench (no WiFi) is unaffected.
- Consumer-side techniques that transfer: a ring of internal-SRAM DMA bands with the converter writing straight into them,
  and one fused YUV→RGB565 + byteswap + crop + rotate pass. PSRAM is not a valid SPI DMA source on the S3.

## Research facts (2026-10-10; [E] = estimate)
- Espressif `esp_h264` SW decoder = **tinyH264 = closed h264bsd fork**, prebuilt `.a`. Constrained baseline, I/P only,
  Annex-B in, I420 out. README: S3R8 640×480 9 fps (11 dual-task), 320×192 23/27; P4 640×480 25/31, 720p 7/10.
  Over h264bsd it adds a deblock filter thread (its "dual task", +22 %), block-copy helpers, and IRAM for
  reconstruct/cavlc/transform/vlc/stream/intra. There is no PIE. Its `libopenh264.a` is encoder-only, with S3 PIE
  quarter-pel MC asm: evidence that PIE MC works on the S3 (closed code, custom licence, do not copy).
- Alt decoders (host x86 proxy, `-O2`, single thread):
  - **TinyH264 upstream** (udevbe 524a702) is identical to our h264bsd within ±1 %.
  - **OpenH264 v2.6.0 decoder, C++ path** is bit-exact and 13–29 % faster on the host, but needs **8.55 MB of heap at
    800×480** (more than the PSRAM), has 3.7× the code, and its frame threads crash. **Don't switch cores.**
  - Borrowed from it: direct-to-picture MC (copy straight into the current picture when cbp = 0, add residual only for
    4×4 blocks with nzc ≠ 0), the alpha/beta = 0 deblock edge skip (QP-dependent), and the inlined bit reader.
- **Corpus caveat:** the search streams are low-QP x264 CBR. Deblock alpha = 0 (a no-op) on 99 / 92 / 37 % of MBs
  (aux_card / aux_map / full_map), so QP-dependent gains are inflated. Check any such gain on a real AA capture.
- P4: HW H.264 encoder only, no decoder. HW JPEG decodes 800×480 at 253 fps. PPA SRM does YUV420→RGB565 plus
  rotate/scale. Prior art payalneg/esp32p4-android-auto (GPL-3.0, don't copy code): 800×480 decode 28 ms, I420 shuffle 17,
  PPA 16 → ~15 fps serial, ~22 fps ceiling with 2 cores.
- S3 PIE YUV420→RGB565 starting point: bitbank2/JPEGDEC `src/s3_simd_420.S` (Apache-2.0, ~8 instr/px) [E]. The S3 LCD_CAM
  YUV→RGB hardware only serves RGB/i80 panels.

## Baselines and noise
- fps follows the boot log line `MSPI Timing: PSRAM timing tuning index`: a bimodal ~0.4–0.7 % difference across boots.
  Indices 4 and 10 are the fast class; 5 is the slow class and the common one. Within one boot, noise is ≤0.02 fps.
- M1 baseline (unmodified h264bsd, decode only; records `docs/baseline-m1.jsonl`, M2 in `docs/baseline-m2.jsonl`):

  | Stream | fast class {4,10} fps | class 5 fps | fast class ms/frame |
  |---|---|---|---|
  | aux_card | 17.24 | 17.12 | 58 |
  | aux_map | 9.73 | 9.69 | 103 |
  | full_map | 5.62 | 5.60 | 178 |
  | aux_map_dark | 9.32 | n/a | 107 |
  | test_640x360 | 12.98 | n/a | |
  | JVT (QCIF) | 57–63 | n/a | |

- Code layout sensitivity was several percent (−4.3 … +1.25 %) with the default 16 KB I-cache. M3 step 0 fixed it: code in
  IRAM (`CONFIG_VJ_IRAM`) cut layout noise to ≤0.11 %. M3's acceptance bar is ±0.3 %.

## Status (newest first)
- **Current tip** (`main`, slow class, `--tuning 5`): **155.19 / 36.98 / 17.60 fps** on aux_card / aux_map / full_map, and
  60.7 fps on the held-out real capture `aa_cluster_heavy` (record `cmp-vj-dual`, `docs/compare-alt.jsonl`). How to read them:
  - Each is decode-only throughput: frames decoded ÷ the summed decode time of the whole stream, on the board, with hash
    checks and I/O outside the timed region. 300 frames per stream (663 for `aa_cluster_heavy`).
  - Per frame that is 6.4 ms (aux_card), 27.0 ms (aux_map) and 56.8 ms (full_map). The consumer needs 30 fps, a 33.3 ms
    budget, so only full_map misses it, and full_map is a worst-case yardstick (a full 800×480 pan), not the AA workload.
  - aux_card: a mostly static turn card. Nearly every MB is skipped, so it measures the skip path (and its 92 ms IDR).
  - aux_map: a panning map in the top-left 640×172 of 800×480 with black margins. This is the realistic AA stream.
  - full_map: the whole 800×480 frame changes. It is the raw decoder-speed yardstick.
  - The 30 fps gate applies to the sustained mean. Worst frames matter too: aa_cluster_heavy has p95 34 ms and max 48 ms.
  - Speed class 5 is the slower PSRAM timing; the fast class reads ~0.4–0.7 % higher. Only compare within one class.
  - For scale, the unmodified h264bsd baseline was 17.12 / 9.69 / 5.60 fps (9× / 3.8× / 3.1× slower). The M3 baseline
    `m3-skiprun` (`docs/baseline-m3.jsonl`) was 155.19 / 36.23 / 17.54; the CAVLC rewrite lifted aux_map +2.1 % and
    full_map +0.3 %.
- **CAVLC rewrite results** (residual per MB, bit window in registers, levels from a table, 2-byte `neighbour_t`; detail in
  bench-log "The CAVLC rewrite"). It targets the heaviest AA frames (top 10 % of `aa_cluster_heavy`: ~19 KB per AU, ~500
  coded MBs, ~25,000 coefficients), where CAVLC residual parse was 37.9 % of a 46.7 ms frame at 613 cycles per 4x4 block.
  - Bar configuration (worker, autoload off, hold on), every hash PASS: p95 44.85 → 41.18 ms, max 67.81 → 59.16 ms,
    53.18 → 56.12 fps; every stream up; internal RAM −661 to −679 B.
  - Search set, bench default: 155.19 / 36.22 / 17.54 → 155.19 / 36.98 / 17.60 fps (−0.00 / +2.09 / +0.32 %).
  - Checked by `host/test_cavlc.c`, which keeps the original decoder as an oracle over 400,000 random residual streams.
  - Left on the table: intra I4x4 (~12 k cycles per MB, ~−3 ms on the max frame), CAVLC in asm (~−8 to −10 ms), and the
    155 ms IDR (107 ms of it is the whole I picture filtered after the decode, since I slices post no rows to the worker).
- esp_h264 dual task on the same bench: 27.94 / 15.77 / 9.13 fps, 20.4 on `aa_cluster_heavy`. VideoJet is 5.55× / 2.34× /
  1.93× faster, 2.98× on the capture. esp_h264 fails the 4-slice aux_map_dark (a board crash with its dual task).
- Merged on `main`, all bit-exact:
  - M3 step 0: code in IRAM, tables in DRAM, D-cache 64 KB / 64 B.
  - Direct skip copy; no per-picture mbStorage clear; skip elision + `changed_mb`; lazy mbLayer clear.
  - Deblock records + edge-less skip; inlined clz bit reader; table-driven CAVLC; the CAVLC rewrite (residual per MB, bit
    window in registers); the skip-run fast path.
  - PIE motion compensation (`VJ_PIE`), PIE write-out, PIE inverse transform; MC stage B C luma kernels.
  - D-cache autoload (`VJ_DCACHE_AUTOLOAD`, opt-in); deblock worker on the other core (`VJ_DEBLOCK_WORKER`, 50 ms
    take-back, a started run is waited for); deblock segments of 4 MBs; the parse pipeline (`VJ_PARSE_PIPELINE`: the
    parser runs in the core-1 worker and feeds a ring of MB records, core 0 reconstructs).
  - Word-at-a-time NAL extraction (run from flash to save internal RAM); cold code in flash.
  - `vj_decoder_set_hold`, `CONFIG_VJ_WORKER_PRIORITY`.
- Library internal RAM (`tools/ram_static.py` section sums plus heap `int_used`, worker stack included): pipeline on
  95,833 B; pipeline off with the worker 99,550 B. Static: 58,493 B with the pipeline on, 56,774 B off. Cost at 800×480:
  heap `psram_used` 1,507,340 B (+589,828 B per extra reference frame); `libvideojet.a` 58,071 B flash.
- Unmerged branches: `m3/rowsready` (finished: `vj_decoder_set_rows_callback` hands rows over as they become final, for
  consumers that convert while decoding; needs a rebase), `m3/recring` (paused WIP: deblock records in a rolling internal
  ring, saves ~18 KB internal RAM in worker builds), `m3/alpha0` (paused WIP: skip the deblock of MBs whose edges are all
  alpha/beta 0; the gain is mostly a low-QP corpus effect), `spike/step0` (throwaway measurement spike, results in
  bench-log).
- Next levers, in order (core 0 binds): the 155 ms IDR (its deblock runs after the decode), intra I4x4, CAVLC in asm, PIE
  deblock, mvpred + records on the parser. Get real AA captures first: at real QP core 1 has no slack. Pending: a
  search-set `tools/score.sh` record of the CAVLC merge for `docs/baseline-m3.jsonl`.
- History: M1 (eval harness, scorer, baseline and profile) and M2 (the public API, Kconfig `VJ_PROFILE` /
  `VJ_ERROR_PRINT`, library-owned memory placement, consumer README, bench `total_us` / `int_used` / `psram_used`) are
  complete and merged. The repo has no remote yet.
