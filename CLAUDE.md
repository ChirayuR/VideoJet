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

## Working conventions
- Ponytail + caveman + superpowers (brainstorm → spec → plan → build).
- Agents: Haiku 5.5 / Sonnet 5.5 for search, research, mechanical work. Opus 5.5 max effort for planning.
  Escalate to an **Opus agent when capability is the bottleneck**: BLOCKED, a fix loop past round 3, or deep-judgment
  work (PIE asm, decoder internals, meta-harness proposers). **If Opus is blocked, escalate to a an external consult agent.**
- Meta-harness drives optimisation (M3). The scorer is the board bench (fps). Hard floor: every frame hash matches the golden.
- Deferred work and every M1 decision live in `docs/superpowers/m1-carryover.md`. Read it before planning M2 or M3.
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
- Board loop: `tools/flash_streams.sh` (only when streams change), then `idf.py -C bench -p /dev/ttyACM0 flash`,
  then `$IDF_PY tools/bench.py --label X --set search`, with `IDF_PY=/path/to/idf-python-env/bin/python`
  (it has pyserial). `bench.py` FAILs unless the boot log's ELF SHA256 matches `bench/build/videojet_bench.elf`
  (`--elf PATH` for another build dir): flash what you built. A `CONFIG_VJ_PROFILE` image needs
  `--profile --elf bench/build-prof/videojet_bench.elf`. Build it with the overlay, so the timed `bench/sdkconfig` never
  sees the switch (`rm -rf bench/build-prof` first; flash it with the same `-B`, then reflash the timed build):
  `idf.py -C bench -B "$PWD/bench/build-prof" -DSDKCONFIG="$PWD/bench/build-prof/sdkconfig"
  -DSDKCONFIG_DEFAULTS="$PWD/bench/sdkconfig.defaults;$PWD/bench/sdkconfig.profile" build`.

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
- 2026-10-10: **M1 complete, merged to `main`** (`7c9c628..3c174ad`).
  - Local repo only, no remote yet: the user will grant GitHub access later.
  - `backups/` (board flash dump, incl. NVS) and `results/` are gitignored. Keep it that way when pushing.
  - What it delivered: the eval harness, a hardened scorer and the baseline + profile.
  - Review: the final review came back clean after one fix wave.
  - Next: M2 (library API + packaging), then M3 (meta-harness search).
- 2026-10-10: the spec is approved.
  - M1 plan: `docs/superpowers/plans/2026-10-10-videojet-m1-eval-harness.md`. It runs through subagent-driven development.
  - The ledger lives in `.superpowers/sdd/`, branch `m1-eval-harness`.
  - Board flash backup: `backups/board_flash_4MB.bin`.
    It covers only the first 960 KB of the old `basemap` area, but that area was erased (0xFF) anyway.
- 2026-10-10 baseline, unmodified h264bsd on the S3, decode-only (M1 smoke run). Noise is ≤0.02 fps within one boot,
  but **bimodal ~0.4–0.7 % across boots**: it follows the boot log line `MSPI Timing: PSRAM timing tuning index`.
  Indices 4 and 10 gave identical fast numbers (the fast class); 5 is ~0.4–0.7 % slower and is the common one (7 of the
  10 boots in `docs/baseline-m1.jsonl`). A code-layout change alone also moves fps by ~±0.5 %.
  So compare candidates A/B/B/A across resets, and only within the same speed class, and accept a win only at ≥1 %.
  `bench.py` records `psram_tuning`. The official baseline (8 boots) is in `docs/bench-log.md`; its records are in
  `docs/baseline-m1.jsonl`.
- **Code layout sensitivity is several percent** (M2 Task 1, 2026-10-10). Identical decoder logic shifted by ~16 B
  measured −3.1 % (aux_map) and −4.3 % (full_map). A +0x51c shift measured +0.6/+0.7 %.
  - Likely cause: the default 16 KB I-cache is smaller than the hot decode code.
  - Consequence: any rebuild that moves code is a ±4 % lottery. M2 acceptance is ±5 % (`compare.py --tol 5`).
  - The ≥1 % bar above is void until **M3 step 0**: pin hot-code placement (libvideojet in IRAM via a
    linker fragment, and/or a 32 KB I-cache), then re-measure the build-to-build noise floor.

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
