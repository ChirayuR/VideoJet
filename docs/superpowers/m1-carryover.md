# M1 carry-over

What the final review of M1 (7c9c628..90fd5db) deferred, by destination, so it outlives the gitignored process
workspace (`.superpowers/`). Nothing here blocks M1: the M1 fixes landed in the final-fix commit. Item tags (I2, m6, ...)
are the review's.

## M2: library API and bench rewrite
- **Timing semantics (I2).** Stop the clock only when `vj_decode` returns a complete frame, and feed exactly one
  pre-indexed AU per call. API contract: no decode work in flight on return; the bench asserts it. Today the untimed
  hash/yield window (about 17 ms per frame) could hide worker-core work.
- **Internal-RAM axis (I4).** Per-stream `int_used` via `heap_caps_monitor_local_minimum_free_size_start/stop`; the
  library's static footprint via `esp_idf_size --archives`; both baselined in `docs/bench-log.md`.
- **Hide the decoder's internals.** `INCLUDE_DIRS` = public API only, `PRIV_INCLUDE_DIRS` = `src`. Today `basetype.h` and
  `h264bsd_util.h` leak `u8`, `u32`, `MIN`, `MAX`, `DEBUG`, `ASSERT` and `NULL` into consumers. The bench includes only the
  public header.
- **Kconfig.** `VJ_PROFILE` and `_ERROR_PRINT` become Kconfig options, `_ERROR_PRINT` off by default (61 `EPRINT` sites
  print from the decode task). The library defines its own `vj_prof` counters (consumers must not define symbols). Drop or
  annotate the dead `H264DEC_OMXDL` bracket.
- **Memory placement.** The library owns it (frames in PSRAM, hot state internal) instead of relying on the consumer's
  malloc settings.
- **Bench rewrite.**
  - Pre-indexed AUs; exact `total_us` per stream.
  - Error rows instead of `ESP_ERROR_CHECK` aborts; bounds checks on the container's count and offsets against the
    partition size.
  - Decode to the end of the stream and require no leftover bytes and no extra pictures.
  - Keep the hash pass identical: it flushes the 32 KB D-cache between frames, so changing it shifts fps.
  - Container identity: `bench.py` checks only that the board's `want` frame count equals the repo golden's line count. A
    container packed from other goldens or streams with the same counts passes. Have `VJ start` print a container checksum
    and compare it with `tools/pack_streams.py`'s output.
- **"fps unchanged" acceptance** = same speed class and |Δ| ≤ 0.6 % per stream against `docs/baseline-m1.jsonl`.
- **Output reordering** option, if streams whose output order differs from decode order are ever needed.

## M3: meta-harness scorer setup
- **Pipeline.**
  1. Path allow-list: `components/h264bsd/**`, plus `bench/sdkconfig.defaults` for ladder step 1 only.
  2. Clean host gate under `timeout`.
  3. Sanitizer gate: ASan + UBSan with `-fno-sanitize=shift-base` (clean on all 11 streams at M1).
  4. Fresh-directory IDF build with `-DSDKCONFIG=<dir>/sdkconfig`: a stale `bench/sdkconfig` beats `sdkconfig.defaults`
     (I3(a)).
  5. Flash, checking the exit code.
  6. `bench.py --elf <dir>/videojet_bench.elf`. Its ELF check ties the run to that file (a 9-hex-digit prefix of the
     sha256 by default); the fresh-directory build is what ties the file to the candidate.
  - A board lock around flash + bench. Transport failures are retried once, correctness failures never.
- **Pin the speed class:** re-reset until the tuning index is in the chosen class (the tuning line prints about 0.1 s after
  reset).
- **Objective:** min fps over `aux_map` and `aux_card` from exact times (not the 2-decimal fps token). Accept only at
  ≥ 1 % (layout noise is ±0.5 %). Repeat dual-core candidates several times (races).
- **Held-out:** board correctness (hashes only) for candidates with ESP-only code paths; a held-out fps baseline per speed
  class (`--set all`) before the first promotion (m6).
- **Host stand-in** for `IRAM_ATTR` / `DRAM_ATTR`, so placement candidates build for the host gate.
- **Scorer hygiene:** `time.monotonic()` timeouts; `capture()`'s result as a `NamedTuple` (it is a 6-tuple now); test
  constants at the top; bounded reason strings; validate `VJP` rows; an fps cross-check against the row times.
- **Proposer prior notes:**
  - `aux_card` (about 51 kb/s) is the skip-heavy best case, not a realistic bitrate.
  - `aux_map_dark` reuses `aux_map`'s route; real AA captures become the true held-out set.
  - The profile's `inter` = MV prediction + motion compensation + residual add/clip + write-out.
- **Corpus tooling when real captures arrive:** `pack_streams` name truncation, collision and zero-stream checks; the ffmpeg
  version recorded beside the goldens; rebuild `vjhash_raw` before `golden.sh`; `CFLAGS` passed explicitly to host builds.

## Parked M1 doc nits (fold into M2's bench-log rewrite)
- In `bench-log.md`, the "98 of 100 rows" and "84 search rows" counts only held through `timed-restore-check`. `bench.jsonl` has grown since.
- The `baseline-prof` and `timed-restore-check` binaries are not named in git.
- "within 0.002 %" should read 0.0022 %.
- The `streams/heldout/README.md` feature list describes the JVT set as a whole, not each stream:
  - refs per stream are 1/5/4/1/4/3;
  - only NRF_MW_E has non-reference pictures;
  - BA1 and BA2 each have a single IDR.

## Decisions taken during M1 (controller rulings; the full process ledger was deleted with the workspace)
- R1. `.gitignore` was created at setup, before the first commit, so `backups/` never enters git.
- R2. The decode loop is duplicated between `host/vjdec.c` and the bench app.
  - A shared API would be premature.
  - Host and board hash equality already cross-checks the two copies.
  - If wrong: an API change has to be edited in two places.
- R3–R5. Every Task 1 gate hole was fixed at once: vacuous pass, header dependencies, full-range goldens, the hash xorshift, input validation, and the vjdec exit status.
- R6. `aux_card` was kept as generated: a skip-heavy best case at about 51 kb/s. Real captures will recalibrate it.
- R7. The user narrowed scope: VideoJet is a decode-only library.
  - The bench moved to `bench/` and results to `results/`.
  - M2 became library packaging; the display work moved to the consumer project.
- R8. Every corpus-affecting fix landed in one regeneration before the baseline was measured: `--cpu-independent`, a real turn flip, `--keyint infinite` plus a frame-count assert.
- R9. The JVT streams are correctness-only. Keep them out of held-out fps aggregates and out of 30 fps headroom arguments.
- R10–R12. `bench.py` was hardened as the M3 scorer:
  - full grid check, fail-fast, visible board errors;
  - a pure `score()` with tests;
  - PSRAM tuning index and idle timeout;
  - exact row format.
- R13. The final fix wave added firmware identity (ELF SHA), the container staleness check, glob dependencies, the baseline snapshot, the layout-noise acceptance rules, and this document.
  - The I2 timer change is deferred to M2's bench rewrite, so M1's numbers stay consistent with M1's code.
- R14. M1 doc-precision nits are parked (see above).
- R15. The frozen M1 plan's profile command lacks `--profile --elf`. CLAUDE.md and `bench-log` carry the correct command.
  - The identity check needs INFO-level boot logs, so M3 must not lower the log level.

## Added during M2 (2026-10-10)
- **M3 step 0, blocking: layout-robust measurement.**
  - M2 Task 1 found that identical decoder logic moves fps by −4.3 % … +0.7 % depending only on code placement. The default 16 KB I-cache is the likely cause.
  - Before any candidate is scored:
    - pin the decoder's hot code: libvideojet `.text` in IRAM via a linker fragment (about 52 KB of internal RAM, inside the 100 KB budget), and/or the 32 KB I-cache;
    - re-measure the build-to-build noise floor with deliberate layout perturbations;
    - only then set M3's acceptance bar. The planned ≥1 % bar is void.
- M2's "fps unchanged" was enforced as ±5 % (ruling R1 of the M2 ledger).
