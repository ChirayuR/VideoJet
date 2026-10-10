We are going to build a video renderer inspired by CubeCoders Jet renderer.

Test it on hardware on esp32s3.

With the goal of optimising as many frames decoded per second as possible.

Use this file as an ongoing file, and optimise your usage.

---

**VideoJet is an ESP-IDF component library: fast H.264 (constrained baseline) decode on the ESP32-S3, and nothing else.**
- The head-unit software-on-S3 display is a **separate consumer project**: the USB display device plus crop/convert/rotate/display on the bar.
  Nothing display-, USB- or protocol-related goes in this repo. `bench/` and `host/` are test apparatus. (user, 2026-10-10)
- Target is the ESP32-S3 only. The P4 is out of scope; P4 facts below are reference only.
- Spec: `docs/superpowers/specs/2026-10-10-videojet-design.md`. Plans live in `docs/superpowers/plans/`.
- API (M2): the component is `components/videojet`; the vendored decoder is private (`h264bsd/`), consumers see
  `include/videojet.h` only. `vj_decode_au(dec, au, len, &frame)` takes **one** Annex-B access unit and returns one I420
  picture; `vj_au_length()` splits a byte stream into AUs. Consumer guide: `components/videojet/README.md`.

## Working conventions
- Ponytail + caveman + superpowers (brainstorm → spec → plan → build).
- Agents: Haiku 5.5 / Sonnet 5.5 for search, research, mechanical work. Opus 5.5 max effort for planning.
  Escalate to an **Opus agent when capability is the bottleneck**: BLOCKED, a fix loop past round 3, or deep-judgment
  work (PIE asm, decoder internals, meta-harness proposers). **If Opus is blocked, escalate to a an external consult agent.**
- Meta-harness drives optimisation (M3). The scorer is the board bench (fps). Hard floor: every frame hash matches the golden.
- Deferred work and every M1 decision live in `docs/superpowers/m1-carryover.md`. Read it before planning M3 (its M2 items
  are closed).
  The scorer identifies the firmware from the INFO-level boot log (`ELF file SHA256`), so never lower the log level.
  Profile runs need `--profile --elf bench/build-prof/videojet_bench.elf`.
- **Read the related projects' notes before asking the user anything** (see Related).

## Consumer context: why the workload looks like this (settled in the head-unit project, do not re-ask)
This belongs to the separate head-unit software-on-S3 project. It defines VideoJet's target stream.
Phone (AA) → Open Headunit (head-unit software) on the G86 head unit (or S24 Self Mode) → AA **second screen**
(AUXILIARY: map or turn card, or CLUSTER) → **head-unit software USB display protocol v1** → ESP32-S3 decodes → 640×172 bar.
- Spec: the head-unit project `origin/feat/second-screen-outputs`, `tools/second-screen/USB-DISPLAY-PROTOCOL.md`.
  Vendor iface FF/4F/44 (found by triple, not VID/PID). GET_INFO → `DISP`, w, h, dpi, max fps, flags.
  Bulk OUT = 16 B frame header (len, ts ms, flags bit0 IDR / bit1 SPS-PPS) + Annex-B AU, ≤16 KiB transfers.
  Optional IN endpoint: byte 0x01 = keyframe request (host turns it into a focus cycle, ≤1 per 5 s).
  Byte vectors: `tools/second-screen/pi-receiver/test_ohud_protocol.py`.
- What head-unit software asks AA for, for a 640×172 panel (`AuxDisplayProfilePolicy`): **H.264 baseline 800×480,
  margins 160 right × 308 bottom, 30 fps fixed**, density from GET_INFO. The picture is the top-left 640×172;
  the rest is blank. Crop, never scale. Active area = 40×11 MBs = 440 of 1500.
- Must keep up with 30 fps: host queue is 90 AUs / 4 MiB, overflow → drops to the next keyframe.
- The branch is NOT in head-unit software main or the rig branch (`the rig branch`). Upstream force-pushes it.
- Real test streams: head-unit software "Network stream" output = raw H.264 on TCP :5000
  (`adb forward tcp:5000 tcp:5000` then `nc 127.0.0.1 5000 > aux.h264`).

## Bench board
Waveshare ESP32-S3-Touch-LCD-3.49 **V2** (the bar board, still flashed with the board firmware).
S3 rev v0.2, 240 MHz, 8 MB octal PSRAM @80 MHz, 16 MB QIO flash, USB-Serial/JTAG on /dev/ttyACM0.
Panel AXS15231B 172×640 portrait (UI landscape 640×172), QSPI on SPI3 at 40 MHz.
VideoJet's bench never drives the panel; the S3 only decodes. The panel constraints below are for the consumer project
(read `the consumer project/docs/display-touch-log.md` there):
- No RASET over QSPI: frames always written from row 0. CASET works (= UI rows); partial TX measured not worth it.
- MADCTL rotation impossible → rotate in software, fused with the RGB565 byteswap, into the band.
- PSRAM is NOT a valid SPI DMA source here → bands live in internal DMA RAM.
- Full frame 220 KB ≈ 13 ms (85 % bus eff). 80 MHz QSPI corrupts pixels, don't.
- `esp_lcd` draw_bitmap measured effectively synchronous on this IO (no overlap won by ping-pong).
- Backlight = TCA9554 ch1 (inverted PWM), LCD reset = ch5. `disp_on_off` is inverted, never call it.
- Panel can latch a bad state across warm reset → 5 s power cycle.
Pins + boot order: `the consumer project/bar/firmware/main/{board_pins.h,display.c}` (reuse, don't re-derive).

## Toolchain
ESP-IDF v5.4.2 at `$IDF_PATH` (`source export.sh`), env `$IDF_ENV`.
No sudo. Host has x264 CLI, gst `avdec_h264` (reference decoder), numpy/PIL. No ffmpeg.
adb: use `~/Android/Sdk/platform-tools/adb` (v37), not `/usr/bin/adb`.
Toolchain traps (found in M1):
- Xtensa `uint32_t` is `unsigned long`. Pass h264bsd's `u32` (`unsigned int`) to its out-params, or GCC 14 errors.
  Host builds never catch this.
- IDF's `-Werror=all` beats `-Wno-error`. Relax one tag at a time with `-Wno-error=<tag>`.
- A component's `file(GLOB ... CONFIGURE_DEPENDS ...)` is a CMake error in IDF's requirements scan (script mode). Guard it
  with `if(CMAKE_BUILD_EARLY_EXPANSION)` (plain glob) / `else()` (see `components/videojet/CMakeLists.txt`).
- esptool/idf.py progress floods output. Pipe through `tr '\r' '\n' | grep -v '%)' | tail`.
- **`idf.py -B dir flash` rebuilds from the current tree** (the M3 spike measured the wrong tree that way): flash
  `@flash_args` from the build dir with esptool, never `idf.py ... flash`. Also, a plain `idf.py -C bench build` writes
  `bench/sdkconfig` (gitignored), which from then on silently overrides `sdkconfig.defaults`: always build in a fresh dir with
  `-DSDKCONFIG=<dir>/sdkconfig`, and delete any `bench/sdkconfig*` that appears.
- Board loop: `tools/flash_streams.sh` (only when streams change), then build fresh and flash exactly that build:
  ```
  rm -rf bench/build && idf.py -C bench -B "$PWD/bench/build" -DSDKCONFIG="$PWD/bench/build/sdkconfig" build
  (cd bench/build && $IDF_PY -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash @flash_args)
  ```
  then `$IDF_PY tools/bench.py --label X --set search`, with `IDF_PY=/path/to/idf-python-env/bin/python`
  (it has pyserial). `bench.py` FAILs unless the boot log's ELF SHA256 matches `bench/build/videojet_bench.elf`
  (`--elf PATH` for another build dir): flash what you built. `--set all` adds the held-out streams (about 8 min).
  A `VJ` row ends `... fps int_min psram_min total_us int_used psram_used`: the exact timed µs and the decoder's
  internal-RAM / PSRAM heap high-water bytes, stack not counted (grammar in `tools/bench.py`).
  `python3 tools/compare.py LABEL` checks a search-set record against the M1 baseline in the same speed class (±5 %);
  `--baseline docs/baseline-m3.jsonl --tol 0.3` checks it against the M3 baseline. It cannot pass a `--set all` record,
  whose held-out streams have no baseline.
  A `CONFIG_VJ_PROFILE` image needs
  `--profile --elf bench/build-prof/videojet_bench.elf`. Build it with the overlay, so the timed build's sdkconfig never
  sees the switch, and reflash the timed build afterwards:
  ```
  rm -rf bench/build-prof
  idf.py -C bench -B "$PWD/bench/build-prof" -DSDKCONFIG="$PWD/bench/build-prof/sdkconfig" \
    -DSDKCONFIG_DEFAULTS="$PWD/bench/sdkconfig.defaults;$PWD/bench/sdkconfig.profile" build
  (cd bench/build-prof && $IDF_PY -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash @flash_args)
  ```

## Jet lessons that transfer
- Consumer project: a ring of internal-SRAM DMA bands with the converter writing straight into them.
  Also one fused pass doing YUV→RGB565 + byteswap + crop + rotate.
- PIE rules, for both the library and the consumer:
  - 16 B-aligned dst; work in 8 px blocks.
  - Scalar tails: PIE silently rounds addresses down.
  - Hand-written loops in asm.
  - Exhaustive scalar-vs-SIMD tests.
- Two cores split by row bands with an adaptive boundary. The worker runs below the caller's priority.
- Hot tables `DRAM_ATTR`, hot code `IRAM_ATTR`, cold data in PSRAM.
- A/B/B/A runs with warmup, accepted only if output hashes match.
- Measured on this board (bar Jet spike): internal-SRAM FB 27.9 ms vs PSRAM 33.7 ms. PIE fill gave
  zero gain. Small allocs eat internal SRAM → `heap_caps_malloc_extmem_enable()`.
- Jet sdkconfig: `-O2` (PERF), DCACHE 64 KB / 64 B line, ICACHE 32 KB, PSRAM OCT 80M,
  `SPIRAM_MALLOC_ALWAYSINTERNAL=16384`, `RESERVE_INTERNAL=32768`. P4: 360 MHz, PSRAM 200M, L2 256 KB / 128 B.

## Candidate decoder core
h264bsd: Android's Hantro baseline decoder, Apache-2.0, plain C (github.com/oneam/h264bsd).

## Research facts (2026-10-10, sources in session; [E] = estimate)
- Espressif `esp_h264` SW decoder = **tinyH264 = closed h264bsd fork**, prebuilt `.a`, no source. Constrained
  baseline, I/P only, Annex-B in, I420 out. README: S3R8 640×480 9 fps (11 dual-task), 320×192 23/27;
  P4 640×480 25/31, 720p 7/10. ≈22 k cycles/MB on S3 (content unknown) [E].
  - Over h264bsd it adds three things (symbols of `libtinyh264.a`, esp_h264 1.4.1); there is no PIE:
    - a deblock filter thread, which is its "dual task" (+22 %): `filterThread`, `h264bsdDecodeSliceDataMultiTask`,
      `h264bsdFilterPictureOneBlock`, `Filter*Cross` edges;
    - block-copy helpers: `h264bsdBlock{Lum,Chrome,ImageLum,ImageChrome,Block}`;
    - IRAM for reconstruct/cavlc/transform/vlc/stream/intra. Inter and deblock are not in IRAM.
  - Its `libopenh264.a` is encoder-only, with S3 PIE asm MC (`McHorVer{01,02,03,10,11,20,22,30}_esp32s3`, `PixelAvg*`).
    So PIE quarter-pel MC is proven on the S3. It is closed code under a custom licence: evidence, not code to copy.
  - the consumer app (an AA minimap on the S3, the user's optimisation target since 2026-10-10) decodes with it today.
- Alt-decoder study (2026-10-10, host x86 proxy, `-O2` single thread; scratchpad `alt-decoders/`):
  - **TinyH264 upstream** (udevbe 524a702): identical to our h264bsd within ±1 %. It only inlines the bit reader (1–2 %).
  - **OpenH264 v2.6.0 decoder, C++ path:**
    - bit-exact on 11/11 streams and 13–29 % faster on the host;
    - but **8.55 MB of heap at 800×480**, which is more than the PSRAM. That is a 3 MiB fixed bitstream buffer, 768 B of coefficients per MB, padded refs and ~2.1 KB/MB of context;
    - code is 3.7× ours, and frame threads are experimental (`-t 2` crashed). **Don't switch cores.**
  - **Borrow:**
    - OpenH264's direct-to-picture MC: when cbp = 0 it copies straight into the current picture, and adds residual in place only for 4×4 blocks with nzc ≠ 0 (`WelsMbInterPrediction`, `IdctFourResAddPred_`). A port covering P_Skip only gave host aux_card −46 %;
    - the alpha/beta = 0 deblock edge skip, which is QP-dependent;
    - the inlined bit reader.
  - **Corpus caveat:** our search streams are low-QP x264 CBR. Deblock alpha = 0 (a no-op) on 99 / 92 / 37 % of MBs (aux_card / aux_map / full_map), so QP-dependent gains are inflated. Real AA captures are still needed.
- P4 has HW H.264 **encoder** only, no HW decoder. P4 HW JPEG decodes 800×480 at 253 fps. PPA SRM does
  YUV420→RGB565 plus rotate/scale.
- Only AA-on-MCU prior art: payalneg/esp32p4-android-auto (GPL-3.0, don't copy code). P4 800×480 full frame:
  decode 28 ms, I420 shuffle 17, PPA 16 → ~15 fps serial, ~22 fps ceiling with 2 cores.
- AA: every frame is a reference (a drop corrupts until the next IDR). IDR comes every ~10–69 s, or <3 s
  after a video-focus cycle. YUV full vs limited range varies by phone → honour the SPS VUI flag.
  Margins on the MAIN display are centred; head-unit software says AUX is top-left → verify on a real capture, keep the crop origin configurable.
- S3 PIE YUV420→RGB565 starting point: bitbank2/JPEGDEC `src/s3_simd_420.S` (Apache-2.0, ~8 instr/px) [E].
  S3 LCD_CAM YUV→RGB HW is for RGB/i80 panels only, so it can't drive our QSPI panel.

## Related (read first)
- `$WORK/the head-unit project` + `~/.claude/projects/-home-chirayu-example-Desktop-Random-the head-unit project/memory/`
- `$WORK/the consumer project` (CLAUDE.md, docs/) + its memory dir.

## Status
- 2026-10-10: **M3 step 0 done** on branch `m3-speed` (Task 1 of `docs/superpowers/plans/2026-10-10-videojet-m3-speed.md`).
  Next: Task 2 (the one-command scorer, which reads `docs/baseline-m3.jsonl`), then the candidate ladder.
  - Changed: libvideojet's code runs from IRAM (`components/videojet/linker.lf`, `CONFIG_VJ_IRAM`, default y, 50,831 B of internal
    RAM) and the bench builds with a 64 KB / 64 B-line D-cache (`bench/sdkconfig.defaults`). No decoder logic changed; the
    consumer README has a "Recommended config" section.
  - New M3 baseline, exact fps (frames·10⁶/total_us), 7 boots all PASS, ELF `e85969831…`, records `m3-base-*` in
    `docs/baseline-m3.jsonl`, aux_card / aux_map / full_map:
    - slow class (idx 5, 5 boots): 22.2074 / 12.7163 / 7.6810 fps = 45.03 / 78.64 / 130.19 ms/frame;
    - fast class (idx 10, 2 boots, no idx 4 drawn): 22.3206 / 12.7590 / 7.6987 fps = 44.80 / 78.38 / 129.89 ms/frame;
    - +29.8 / +31.5 / +37.8 % over M2 in the slow class (`docs/bench-log.md`, M3 step 0).
  - Noise now: layout at most 0.11 %, same binary at most 0.0019 % on the 7 baseline boots (0.004 % is the spike's bound), class gap
    +0.51 / +0.34 / +0.23 %. M3's bar: every search stream ≥ −0.3 % and one ≥ +0.3 %, within one class.
  - Cost axes: heap `int_used` 13,544 B + IRAM 50,831 B = 64,375 B of library internal RAM (budget ≤ 100 KB, stack still
    unmeasured); `psram_used` 1,507,340 B unchanged; bench `int_min` 271,699 B (IRAM 50,944 B and the bigger D-cache 31,996 B).
  - New trap: `idf.py -B dir flash` rebuilds from the current tree; flash `@flash_args` (Toolchain traps).
- 2026-10-10: **M2 complete** on branch `m2-library-api` (`19c1a23..89bd8c9`, then the docs commit `d7e3516` and its fix
  round). Next was M3 step 0 (done, first entry above), then the meta-harness search.
  - Delivered: the public API, one component with a private decoder, Kconfig `VJ_PROFILE` / `VJ_ERROR_PRINT`, library-owned
    memory placement, the consumer README, and a bench that reports exact `total_us` and the decoder's `int_used` / `psram_used`.
  - The final fix wave `f72f591` (an AU that leaves a picture unfinished returns VJ_ERR_STREAM) is board-verified:
    record `m2-f72f591`, ELF `3cc14a9cb`, PASS, −0.06 / −0.21 / −0.71 % at idx 10, inside the ±5 % band.
  - Final firmware (app `89bd8c9`, ELF `6a270d064`, record `m2-final-all`: PASS, all 11 streams). Search fps against the M1
    baseline of the same class: −0.06 / −0.21 / −0.36 % (aux_card / aux_map / full_map), inside the ±5 % M2 bar. The
    committed M2 builds spread −2.14 … +1.25 % with no decoder algorithm changed: that is code layout (below).
  - Cost axes at 800x480, M3's baseline (`docs/bench-log.md`, M2 section; records in `docs/baseline-m2.jsonl`): heap
    `int_used` 13,544 B, heap `psram_used` 1,507,340 B (+589,828 B per extra reference frame); `libvideojet.a` static: 0 B
    IRAM, 0 B DRAM, 58,071 B flash. The decode task's stack is not measured by any axis.
  - Held-out fps at index 5 (slow class, no same-class baseline): aux_map_dark 9.27, test_640x360 12.99; JVT QCIF
    56.5–62.5 (correctness only).
- 2026-10-10: **M1 complete, merged to `main`** (`7c9c628..3c174ad`).
  - Local repo only, no remote yet: the user will grant GitHub access later.
  - `backups/` (board flash dump, incl. NVS) and `results/` are gitignored. Keep it that way when pushing.
  - What it delivered: the eval harness, a hardened scorer and the baseline + profile.
  - Review: the final review came back clean after one fix wave.
- 2026-10-10: the spec is approved.
  - M1 plan: `docs/superpowers/plans/2026-10-10-videojet-m1-eval-harness.md`. It runs through subagent-driven development.
  - The ledger lives in `.superpowers/sdd/`, branch `m1-eval-harness`.
  - Board flash backup: `backups/board_flash_4MB.bin`.
    It covers only the first 960 KB of the old `basemap` area, but that area was erased (0xFF) anyway.
- 2026-10-10 baseline, unmodified h264bsd on the S3, decode-only (M1 smoke run). Noise is ≤0.02 fps within one boot,
  but **bimodal ~0.4–0.7 % across boots**: it follows the boot log line `MSPI Timing: PSRAM timing tuning index`.
  Indices 4 and 10 gave identical fast numbers (the fast class); 5 is ~0.4–0.7 % slower and is the common one (7 of the
  10 boots in `docs/baseline-m1.jsonl`). Compare candidates A/B/B/A across resets, and only within the same speed class.
  (M1 also put code layout at ~±0.5 % and the acceptance bar at ≥1 %: both are void, see the layout bullet below.)
  `bench.py` records `psram_tuning`. The official baseline (8 boots) is in `docs/bench-log.md`; its records are in
  `docs/baseline-m1.jsonl` (M2's two final records: `docs/baseline-m2.jsonl`).

  Baseline fps by speed class. The held-out rows come from the smoke boot, whose search fps put it in the fast class.

  | Stream | fast class {4,10} fps | class 5 fps (common) | fast class ms/frame |
  |---|---|---|---|
  | aux_card | 17.24 | 17.12 | 58 |
  | aux_map | 9.73 | 9.69 | 103 |
  | full_map | 5.62 | 5.60 | 178 |
  | aux_map_dark | 9.32 | n/a | 107 |
  | test_640x360 | 12.98 | n/a | |
  | JVT (QCIF) | 57–63 | n/a | |

  A near-static card still costs ~9k cycles per skipped MB, so skip elision is the first lever.
- **Code layout sensitivity is several percent** (M2 Task 1, 2026-10-10). With no decoder algorithm changed, fps moved by
  −4.3 … +1.25 % against the same-class baseline, observed over the Task 1 controls and the committed M2 builds
  (`docs/bench-log.md`): the M1 logic shifted by ~16 B gave −3.1 % (aux_map) and −4.3 % (full_map), the M2 wrapper linked in
  front of it (+0x51c) +0.6 / +0.7 %, the final M2 build −0.06 / −0.21 / −0.36 %.
  - Likely cause: the default 16 KB I-cache is smaller than the hot decode code.
  - Consequence: any rebuild that moves code has landed anywhere in that band so far. M2 acceptance was ±5 %
    (`compare.py --tol 5`).
  - Resolved by **M3 step 0** (first Status entry): libvideojet runs from IRAM (`CONFIG_VJ_IRAM`), and layout noise is now at most 0.11 %
    (same binary at most 0.004 % in the spike, 0.0019 % on the baseline boots). M1's ≥1 % bar stays void; M3's bar is ±0.3 % (plan, Global Constraints).
