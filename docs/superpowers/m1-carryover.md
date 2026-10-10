# M1 carry-over

What the final review of M1 (7c9c628..90fd5db) deferred, by destination, so it outlives the gitignored process
workspace (`.superpowers/`). Nothing here blocks M1: the M1 fixes landed in the final-fix commit. Item tags (I2, m6, ...)
are the review's.

## M2: library API and bench rewrite (closed 2026-10-10)
Each item carries the commit that closed it (branch `m2-library-api`). What M2 did not do moved to the M3 list below.
- **Timing semantics (I2). Closed by `291405e`, `d691aff`.** `vj_decode_au()` takes exactly one pre-indexed AU per call and the
  bench stops the clock when it returns. `videojet.h` states the contract: no decode work is left running when a call returns.
  The bench does not assert it: M2's decoder has no worker, so nothing can run after a return. A multi-core M3 candidate must
  keep the contract.
- **Internal-RAM axis (I4). Closed by `89bd8c9` and the M2 docs commit (the one after `89bd8c9`).** `int_used` / `psram_used`
  per stream from the IDF heap monitor (`89bd8c9`); both, and `libvideojet.a`'s static footprint from
  `esp_idf_size --archives`, are baselined in `docs/bench-log.md`, section "M2 (library API)".
- **Hide the decoder's internals. Closed by `c9e6510`.** `INCLUDE_DIRS` = `include` (the public API only), `PRIV_INCLUDE_DIRS` =
  `src` and the decoder directory. The bench includes only `videojet.h`. (`9137edb`: first-party sources build with strict
  warnings, the one `-Wno-error` is on the vendored sources only.)
- **Kconfig. Closed by `c9e6510`.** `CONFIG_VJ_PROFILE` and `CONFIG_VJ_ERROR_PRINT`, both off by default (the bench enables
  profiling through `bench/sdkconfig.profile`); the library defines its own `vj_prof` counters. The dead `H264DEC_OMXDL`
  bracket is annotated in `h264bsd/vj_prof.h`, not removed.
- **Memory placement. Closed by `c9e6510`, `9137edb`.** `vj_alloc()` puts buffers above 16 KiB in PSRAM first and smaller ones in
  internal RAM first, whatever the consumer's malloc settings. `9137edb`: it asks for `MALLOC_CAP_DEFAULT` like IDF's malloc,
  so it cannot drain the DMA reserve (checked on the board).
- **Bench rewrite. Closed by `291405e`, `89bd8c9`, except container identity (moved to M3).**
  - Pre-indexed AUs (`291405e`); exact `total_us` per stream (`89bd8c9`).
  - Error rows instead of `ESP_ERROR_CHECK` aborts, and bounds checks on the container's count and offsets against the
    partition size (`89bd8c9`: `error=read`, `VJ error=container`, 64-bit sums).
  - Decode to the end of the stream, no leftover bytes and no extra pictures: `291405e` (`error=noau` for bytes left that hold
    no start code), `89bd8c9` (`error=extra_frames` for a picture past the golden's count).
  - The hash pass is unchanged since M1 (`components/vj_hash` is untouched), so it still flushes the D-cache between frames.
  - Container identity: **not done**, see the M3 list.
- **"fps unchanged" acceptance. Closed:** enforced as ±5 % per stream, same speed class (ruling R1 of the M2 ledger;
  `tools/compare.py`, default set by `d691aff`), because the ±0.6 % rule sits inside the layout band. The final firmware
  measured −0.06 / −0.21 / −0.36 % (`docs/bench-log.md`, M2 section).
- **Output reordering** option: not done, and not needed so far. Pictures come out in decode order (`videojet.h`).

## M3: meta-harness scorer setup
- **Pipeline.**
  1. Path allow-list: `components/videojet/h264bsd/**`, plus `bench/sdkconfig.defaults` for ladder step 1 only.
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
- **Objective:** min fps over `aux_map` and `aux_card` from exact times (`total_us`, exact since M2; not the 2-decimal fps
  token). The acceptance bar comes from M3 step 0 below: M1's "≥ 1 % (layout noise is ±0.5 %)" is void. Repeat dual-core
  candidates several times (races).
- **Held-out:** board correctness (hashes only) for candidates with ESP-only code paths; a held-out fps baseline per speed
  class (`--set all`) before the first promotion (m6). M2 left one for the slow class only: `m2-final-all`, on the committed
  M2 firmware (`docs/bench-log.md`).
- **Host stand-in** for `IRAM_ATTR` / `DRAM_ATTR`, so placement candidates build for the host gate.
- **Scorer hygiene:** `time.monotonic()` timeouts; `capture()`'s result as a `NamedTuple` (it is a 6-tuple now); test
  constants at the top; bounded reason strings; validate `VJP` rows; an fps cross-check against the row times.
- **Proposer prior notes:**
  - `aux_card` (about 51 kb/s) is the skip-heavy best case, not a realistic bitrate.
  - `aux_map_dark` reuses `aux_map`'s route; real AA captures become the true held-out set.
  - The profile's `inter` = MV prediction + motion compensation + residual add/clip + write-out.
- **Corpus tooling when real captures arrive:** `pack_streams` name truncation, collision and zero-stream checks; the ffmpeg
  version recorded beside the goldens; rebuild `vjhash_raw` before `golden.sh`; `CFLAGS` passed explicitly to host builds.
- **Carried over from M2 (the task reviews):**
  - **Host harness for the bench's C control flow**, committed as a pre-flash gate: `bench/main/bench_main.c` built for the
    host against a stub IDF and a heap-monitor model, run under ASan. M2 Task 3's reviewer wrote one (about 150 lines, 30 fault
    cases, 6 of 6 mutants caught); it was not committed.
  - **Scorer:**
    - require `int_used` and `psram_used` > 0 (the bench prints 0 only if the heap monitor could not start, and `score()`
      accepts it);
    - check `total_us` against `idr_us + p_mean_us × (frames − 1)`: on all 28 rows of `m2-t3-final` and `m2-final-all` the
      difference is in [0, frames − 1) µs, the truncation of `p_mean_us`; also check fps against `frames·10⁶ / total_us`
      (it matches on those 28 rows). Digit-level damage to `total_us`, `int_used` and `psram_used` is accepted today.
  - **Bench container:** `data_len == 0` or `frames == 0` reaches `malloc(0)` and prints a mislabelled `error=nomem`, and an
    entry with `set > 1` is silently never run. Make both container violations (`VJ error=container`).
  - **Container identity** (from the M2 bench-rewrite list, not done): `bench.py` checks only that the board's `want` frame
    count equals the repo golden's line count, so a container packed from other goldens or streams with the same counts
    passes. Have `VJ start` print a container checksum and compare it with `tools/pack_streams.py`'s output.
  - **README example regression guard:** the example in `components/videojet/README.md` was checked by extracting it and
    running it (ASan, bit-exact on all 11 streams), but nothing committed repeats that. Promote the extract-and-run check to a
    host make target.
  - **`vj_alloc.h` comment:** it says the DMA reserve serves only explicit DMA requests. In IDF it also serves FreeRTOS stacks
    and `MALLOC_CAP_INTERNAL` requests without `MALLOC_CAP_DEFAULT`.
  - **Fault-injection (OOM) coverage in `make check`:** M2 Task 2's malloc fault-injection sweep (over 100 allocations: 0 leaks,
    0 wrong statuses) was scratch-only.
  - **`make check` prints 8 deliberate `ERROR:` lines** (the damage tests, built with `-D_ERROR_PRINT`). A CI grep for
    "ERROR" would trip on them: grep the exit status instead, or quiet the tests.

## Parked M1 doc nits (closed by the M2 docs commit, the one after `89bd8c9`)
- `bench-log.md`: the "98 of 100 rows" and "84 search rows" counts are now qualified as "through `timed-restore-check`".
- The `baseline-prof` and `timed-restore-check` records carry no `elf_sha`, so `bench-log.md` now says their binaries are not
  recorded.
- "within 0.002 %" now reads 0.0022 % (recomputed from row times: 0.002245 % at worst, on full_map).
- The `streams/heldout/README.md` feature list now says it describes the JVT set as a whole, not each stream:
  - refs per stream are 1/5/4/1/4/3 (the list said 3-5; read from each stream's SPS);
  - only NRF_MW_E has non-reference pictures (66 of its 100);
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
- R14. M1 doc-precision nits were parked, then closed by the M2 docs commit (see above).
- R15. The frozen M1 plan's profile command lacks `--profile --elf`. CLAUDE.md and `bench-log` carry the correct command.
  - The identity check needs INFO-level boot logs, so M3 must not lower the log level.

## Added during M2 (2026-10-10)
- **M3 step 0, blocking: layout-robust measurement.**
  - M2 Task 1 found that identical decoder logic moves fps by −4.3 % … +0.7 % depending only on code placement. The default 16 KB I-cache is the likely cause.
  - Before any candidate is scored:
    - pin the decoder's hot code: libvideojet `.text` in IRAM via a linker fragment (50,981 B measured, `docs/bench-log.md`:
      64,525 B of internal RAM with the decoder's 13,544 B heap, inside the 100 KB budget), and/or the 32 KB I-cache;
    - re-measure the build-to-build noise floor with deliberate layout perturbations;
    - only then set M3's acceptance bar. The planned ≥1 % bar is void.
- M2's "fps unchanged" was enforced as ±5 % (ruling R1 of the M2 ledger). The final firmware measured −0.06 / −0.21 / −0.36 %
  (`docs/bench-log.md`, M2 section); the committed M2 builds spread −2.14 % … +1.25 %.
