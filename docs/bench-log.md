# Bench log

fps = frames / total decode time, decode-only. Board: Waveshare 3.49 V2, S3 240 MHz, octal PSRAM 80 MHz.
Correctness floor: every hash passes. Search set only. Each value is the mean over the run's rows
(run 0 and run 1 of a boot), then over the boots at that tuning index.
Source: `results/bench.jsonl` (gitignored), records `baseline-r1` to `baseline-r8`: 8 boots, each `tools/bench.py
--set search`, all PASS (6 rows each), each record carries its `psram_tuning`. The records this log cites
(`baseline-r1` to `baseline-r8`, `baseline-prof`, `timed-restore-check`) are versioned, unmodified, in
`docs/baseline-m1.jsonl`. They predate the `elf_sha` and `app_version` fields, so no record names its binary. The timed
baseline image is named below (Results), from its boot log; the binaries of `baseline-prof` and `timed-restore-check` are
not recorded anywhere.
"From row times" below means fps recomputed as frames·10⁶ / (idr_us + p_mean_us·(frames−1)). The firmware prints fps
with 2 decimals only, and p_mean_us is the mean truncated to a whole µs. Through `timed-restore-check` (when `bench.jsonl`
held 100 rows; it has grown since) the formula reproduces the printed fps on 98 of the 100 rows: all 84 search-set rows
(so all 60 rows of the versioned records) and 14 of the 16 held-out rows of the `m1-smoke` record. The two misses are that
record's BA_MW_D and MIDR_MW_D run 1 (printed 60.41 and 60.44, formula 60.42 and 60.45: the truncation). Since M2 a row
carries `total_us`, the exact sum of the timed µs, and fps is computed from it, so M2 rows need no reconstruction.

## Noise
- within one boot: max |run0 − run1| fps per stream = 0.00 for aux_map, aux_card and full_map (printed fps, 8 boots).
  From row times: 0.0001 aux_map, 0.0003 aux_card, 0.00003 full_map (at most 0.0017 %).
- across boots: PSRAM tuning index observed = {5, 10}; per-index mean fps per stream below.
  - Within an index the boots agree: the 7 boots at index 5 all print 17.12 / 9.69 / 5.60 (aux_card / aux_map / full_map),
    and from row times the boot means differ by at most 0.00015 fps (aux_card; about 0.0022 % at worst, on full_map:
    0.002227 %).
  - Between indexes: spread = 0.70 % aux_card, 0.49 % aux_map, 0.36 % full_map, from row times (index 10 faster than 5).
    The printed fps give 0.70 / 0.41 / 0.36 %, rounding included.
  - So the index shifts fps by 0.4–0.7 %, and within an index the noise is about 0.0022 % at most: over a hundred times
    smaller.
- index frequency: 7 of the 8 baseline boots landed on 5, one (r5) on 10. Over the 10 records in `docs/baseline-m1.jsonl`
  (those 8, `baseline-prof` and `timed-restore-check`) the index was 5 ×7, 10 ×2, 4 ×1.
  Index 4 has no row below, because the 8 baseline boots did not draw it. Record `timed-restore-check` (the timed
  firmware rebuilt after the profile build, tuning 4) printed 17.24 / 9.73 / 5.62 (aux_card / aux_map / full_map), the same
  as the index 10 row (from row times, about 0.0022 % apart at most: max 0.002245 %). So 4 and 10 form one fast speed class,
  and 5 is the slower, more common one. The `m1-fix1*` records predate the `psram_tuning` field.
  Through `m2-final-all`, the 34 records of `bench.jsonl` that carry an index (M1 and M2) show 4 ×9, 5 ×19, 10 ×6: no other
  index has appeared, and `tools/compare.py` knows only these three.
- code layout: **moving code alone shifts fps by several percent, not by ±0.5 %.**
  - M1 saw the effect first but measured it too small. The profile firmware (`baseline-prof`) and the timed one
    (`baseline-r5`) both booted at index 10 and do the same decoder work, yet from row times the profile firmware is −0.43 %
    on aux_card, +0.22 % on aux_map and +0.50 % on full_map (printed fps: −0.46 / +0.31 / +0.53 %). That mixed-sign
    difference comes from code placement (and the counters), not from the speed class.
  - M2 Task 1 measured it with the decoder's logic unchanged (records `ctl-m1-code` and `ctl-m1-shift16` in
    `results/bench.jsonl`; Δ against the same-class baseline, aux_card / aux_map / full_map):
    - the untouched M1 sources, rebuilt (ELF 758558c2b, index 4): 0.00 / 0.00 / 0.00 %: in this control, a rebuild alone
      changed nothing;
    - the M1 logic with the decoder's code shifted by 16 B (ELF ae66914a5, index 10; other code moved too):
      −0.12 / −3.08 / −4.27 %;
    - the M2 wrapper linked in front of the decoder, which moved the decoder by about +0x51c (ELF a91706eed): +0.06 / +0.62 /
      +0.71 % at index 4 (+0.12 / +0.62 / +0.71 % at index 5).
    Over these controls and every committed M2 firmware (table in M2 below), where no decoder algorithm changed, the observed
    band is **−4.3 % … +1.25 %** against the same-class baseline.
  - Likely cause: the default 16 KB I-cache is smaller than the hot decode code, so which functions share cache lines decides
    the fps. This is a hypothesis; nothing here tested it.
  - The within-index noise above (about 0.0022 %) is same-binary, same-speed-class noise, i.e. how well one binary repeats.
    The build-to-build noise floor was unknown until M3 step 0 pinned the layout: with the code in IRAM it is at most 0.11 %
    (M3 step 0 section below).
- rule for M3: compare candidates A/B/B/A across resets, and only within the same tuning index (or report both).
  An A/B pair from boots at different indexes can fake a 0.4–0.7 % gain or loss. `bench.py` cannot choose the index:
  every reset draws it afresh, so a boot at the odd index has to be re-run, or compared only with its own index.
- acceptance rules:
  - M2's "fps unchanged" was enforced as the same speed class and |Δ| ≤ 5 % per stream against `docs/baseline-m1.jsonl`
    (`tools/compare.py`, default `--tol 5`; ruling R1 of the M2 plan, Global Constraints in
    `docs/superpowers/plans/2026-10-10-videojet-m2-library-api.md`). M1's ≤ 0.6 % sits inside the layout band above: the
    first M2 build (Task 1, vendored decoder untouched) measured +0.62 % on aux_map and +0.71 % on full_map and missed it.
  - M3 step 0 (done, M3 step 0 section below): libvideojet's code is pinned in IRAM and the noise was re-measured with
    deliberate layout perturbations (`-falign-functions=64`, a +0x26c shift): layout noise at most 0.11 %, same binary at most
    0.004 % (0.0019 % on the 7 step-0 boots). M3's bar is ±0.3 %: every search stream at least −0.3 % and one at least +0.3 %, from exact fps, within one speed
    class. M1's ≥ 1 % bar stays void.

## Results
| date | label | tuning | aux_map fps | aux_card fps | full_map fps | aux_map IDR ms / P p95 ms | int_min KB | notes |
|---|---|---|---|---|---|---|---|---|
| 2026-10-10 | baseline (unmodified h264bsd, IDF defaults + -O2) | 5 | 9.69 | 17.12 | 5.60 | 139.36 / 113.69 | 346.3 | 7 boots (r1–r4, r6–r8) |
| 2026-10-10 | baseline (same binary) | 10 | 9.73 | 17.24 | 5.62 | 139.06 / 113.25 | 346.3 | 1 boot (r5) |

int_min is 354,655 B in every timed-firmware row of every boot (KB = 1024 B; the profile firmware, `baseline-prof`, reads
354,615 B); the M2 timed builds before Task 3 read 354,655 B too. Per-stream P p95 and IDR on the other streams:
index 5 aux_card 60.36 / 116.78 ms (P p95 / IDR), full_map 197.31 / 217.46 ms; index 10 aux_card 59.96 / 116.46 ms,
full_map 196.62 / 217.19 ms.
Binary: the M1 Task 4 image, not rebuilt for this log (boot log: app version `88ed2d6-dirty`, compiled `Oct 10 2026 04:13:34`,
ELF SHA256 `c31a1eec3…`).
M2 rows are in the M2 section below; their int_min and psram_min are not comparable byte for byte with these (see
"Row format").

## Baseline profile
Where a frame's decode time goes, per search stream: the `VJP` lines of `results/bench.jsonl`, record `baseline-prof`
(one boot, PASS, 6 rows, `psram_tuning` 10, the fast class). It is a separate firmware, not the timed one. Since M2 it is
built with the profile overlay (counters in `components/videojet/h264bsd/vj_prof.h`, enabled by `CONFIG_VJ_PROFILE`, off by
default; the M1 record `baseline-prof` was built with `-DVJ_PROFILE=1`, the switch that overlay replaced):
```
rm -rf bench/build-prof
idf.py -C bench -B "$PWD/bench/build-prof" -DSDKCONFIG="$PWD/bench/build-prof/sdkconfig" \
  -DSDKCONFIG_DEFAULTS="$PWD/bench/sdkconfig.defaults;$PWD/bench/sdkconfig.profile" build
```
It is flashed with esptool `write_flash @flash_args` from `bench/build-prof` (not `idf.py -B ... flash`, which rebuilds from
the current tree), run with
`tools/bench.py --label baseline-prof --set search --profile --elf bench/build-prof/videojet_bench.elf` (bench.py now
refuses a `VJP` line without `--profile`, and checks the boot log's ELF SHA256 against `--elf`), then the default build
flashed back. Each stage is a CCOUNT delta around one call, summed over the stream's 300 frames
(IDR included, like fps) and divided by 300. Run 0 is shown; run 1 differs by at most 1 µs.
- parse = `h264bsdDecodeMacroblockLayer` (CAVLC), once per coded macroblock.
- recon = `h264bsdDecodeMacroblock`, once per macroblock, skipped ones included: residual, intra, inter, write-out.
- inter = `h264bsdInterPrediction`, a part of recon, once per inter macroblock (skipped ones included). It is motion-vector
  prediction + motion compensation (`h264bsdPredictSamples`) + residual add/clip + write-out into the picture
  (`h264bsdWriteOutputBlocks`, or `h264bsdWriteMacroblock` for a skipped macroblock): not motion compensation alone.
- deblock = `h264bsdFilterPicture`, once per picture.
- other = mean frame time − parse − recon − deblock: NAL and slice-header parsing, skip runs, DPB, and on the IDR the SPS/PPS and allocation.

| stream | mean frame µs | parse µs (%) | recon µs (%) | of which inter µs (%) | deblock µs (%) | other µs (%) |
|---|---|---|---|---|---|---|
| aux_card | 58,269 | 531 (0.9 %) | 44,864 (77.0 %) | 41,891 (71.9 %) | 8,203 (14.1 %) | 4,671 (8.0 %) |
| aux_map | 102,498 | 14,429 (14.1 %) | 64,079 (62.5 %) | 50,275 (49.0 %) | 17,549 (17.1 %) | 6,441 (6.3 %) |
| full_map | 177,038 | 27,883 (15.7 %) | 105,726 (59.7 %) | 84,257 (47.6 %) | 34,963 (19.7 %) | 8,466 (4.8 %) |

Counter cost: the profile firmware printed 17.16 / 9.76 / 5.65 fps (aux_card / aux_map / full_map) against 17.24 / 9.73 / 5.62
for the timed baseline at index 10 (Results table): within about 0.5 %, mixed sign. That is inside the code-layout band
(Noise section: several percent), so the counter cost cannot be told apart from it. The timed firmware rebuilt with these
changes and flashed back printed 17.24 / 9.73 / 5.62 at index 4 (record `timed-restore-check`, no `VJP` lines), the same
numbers as the index 10 row.

Reading: `inter` (MV prediction + motion compensation + residual add/clip + write-out) dominates all three streams; this
profile does not split motion compensation from the write-out. aux_card is skip-heavy (parse is 0.9 % of the frame, which
suggests almost no macroblock is coded; the counters do not count macroblocks, so that is inferred), and its 72 % `inter` is
mostly the skipped-macroblock copy path (MV prediction, reference copy, `h264bsdWriteMacroblock`); deblock is next at 14 %.
aux_map and full_map spend about half the frame in `inter` (49 % / 48 %); the rest is deblock 17 % / 20 %, parse 14 % / 16 %,
recon minus `inter` (residual dequantisation and inverse transform, intra prediction with its write-out) 13.5 % / 12 %,
and other 6 % / 5 %.

## M2 (library API)
The M2 firmware is the committed tree at `89bd8c9`: one `videojet` component with a private decoder, Kconfig switches, and the
bench on the public API. Two records ran on it, both PASS and both tied by `bench.py` to its ELF (SHA256 `6a270d064…`, app
version `89bd8c9`): `m2-t3-final` (search set, 6 rows, PSRAM tuning index 4) and `m2-final-all` (all 11 streams, 22 rows,
index 5). The held-out fps and every stream's memory come from `m2-final-all`, the search comparison from `m2-t3-final`
(against the M1 baseline, `tools/compare.py` cannot pass a `--set all` record: the held-out streams have no baseline
there). These two records are versioned, unmodified, in `docs/baseline-m2.jsonl`: M3's baseline, as `docs/baseline-m1.jsonl`
is M1's. (`compare.py --baseline docs/baseline-m2.jsonl` needs a like-for-like run: its slow-class baseline holds all 11
streams, so a `--set all` run, and its fast-class one only the 3 search streams.) Every measured number below comes from
`results/bench.jsonl` (gitignored; only those two records are versioned) or from the command shown with it. The exception is
the per-allocation sizes in the Memory section, which come from a scratch log.

### Row format
The firmware prints one row per stream and run, key=value, on one line (`tools/bench.py` has the exact grammar); a board
error adds ` error=E` at the end. As captured (`m2-final-all`, aux_card run 0):
```
VJ run=0 set=search stream=aux_card frames=300/300 fail=0 first_fail=-1 idr_us=116521 p_mean_us=58267 p50_us=57843 p95_us=60399 max_us=83483 fps=17.11 int_min=354639 psram_min=6808212 total_us=17538564 int_used=13544 psram_used=1507340
```
`total_us`, `int_used` and `psram_used` are new in M2, and `int_min` and `psram_min` shifted:
- `total_us`: the exact sum of every timed µs of the stream, parameter-set-only and failing AUs included. fps is computed from it.
- `int_used`, `psram_used`: the decoder's heap high-water marks in bytes, from the IDF heap monitor
  (`heap_caps_monitor_local_minimum_free_size_start/stop`) around one stream's decode: how far the free internal and PSRAM heap
  fell below its level just before `vj_decoder_create()`. The bench's own buffers and AU index are allocated first, so they
  are not counted, and neither is the stack the decoder's calls use (see Static footprint).
- `int_min`, `psram_min` keep their boot-wide meaning (lowest free size since boot) but no longer match M1 byte for byte. With
  the monitor linked (M2 Task 3), `int_min` reads 354,639 B, not the 354,655 B of the M1 and M2 timed builds before Task 3: −16 B,
  because the monitor pulls in IDF's `min_free_bytes_monitoring` (16 B of `.data`), a link-time effect. `psram_min` is 28 B
  lower (the monitor's own table), and 2,436 B lower since M2 Task 1 (the AU index): aux_card run 0 reads 6,810,676 B in M1,
  6,808,240 B in M2 Task 1 and 2, 6,808,212 B now. None of it is a placement change.

### fps against the M1 baseline
M2 acceptance is |Δ| ≤ 5 % per stream in the same speed class (ruling R1 of the M2 plan; Noise above).
`python3 tools/compare.py m2-t3-final`, as the M2 script printed it (from the fps tokens):
```
aux_card                    17.23 vs    17.24 fps (fast)  -0.06 %  ok
aux_map                      9.71 vs     9.73 fps (fast)  -0.21 %  ok
full_map                     5.60 vs     5.62 fps (fast)  -0.36 %  ok
PASS
```
`m2-final-all` is the same firmware at index 5 (slow class): 17.11 / 9.67 / 5.58 fps against 17.12 / 9.69 / 5.60, the same
−0.06 / −0.21 / −0.36 % (the M2 `compare.py` printed FAIL for that record only because of the held-out "no baseline" lines; it now
prints `ERROR: <stream>: no baseline` for them and exits 2). Exactly,
from `total_us` against the baseline from row times: −0.051 / −0.205 / −0.447 % (fast) and −0.061 / −0.201 / −0.441 % (slow).
The two classes agree: the Δ belongs to the binary, and it sits well inside the layout band (Noise above).

Every committed M2 firmware against the same-class baseline, as `compare.py` computed it from the printed fps (aux_card /
aux_map / full_map, %). No decoder algorithm changed in any of them: the spread is code placement (Noise above). The two
controls are M1 logic. Labels repeat in `results/bench.jsonl` (`m2-t1` is on four records, on two ELFs), and `compare.py LABEL`
reads only the last record with a label, so a record is identified by label, ELF and index. The rows below are
the M2 `compare.compare()` over each such record, from the fps tokens; the CLI then reproduced all of them except the first
(the `m2-t1` index-4 record). It no longer does: since M3 Task 2 `compare.py` computes exact fps from `total_us` wherever a row
has it, so `m2-t3-final` now prints −0.076 / −0.157 / −0.438 % (exact run against M1's token baseline), while the table's
2-decimal values come from the fps tokens.

| firmware | record | ELF | index (class) | aux_card | aux_map | full_map |
|---|---|---|---|---|---|---|
| `291405e` public API | `m2-t1` | a91706eed | 4 (fast) | +0.06 | +0.62 | +0.71 |
| `291405e` public API | `m2-t1` | a91706eed | 5 (slow) | +0.12 | +0.62 | +0.71 |
| `d691aff` API fix | `m2-t1-fix1` | 5b446cd93 | 5 (slow) | +0.12 | +1.03 | +1.25 |
| `c9e6510` one component, Kconfig, placement | `m2-t2-final` | 970f7357f | 5 (slow) | +0.18 | −0.93 | −1.43 |
| `9137edb` S3 sizing, DMA-safe alloc | `m2-t2-fix1-final` | dc5ae7a67 | 4 (fast) | +0.12 | −1.34 | −2.14 |
| `89bd8c9` bench measures (final) | `m2-t3-final` | 6a270d064 | 4 (fast) | −0.06 | −0.21 | −0.36 |
| `89bd8c9` (final) | `m2-final-all` | 6a270d064 | 5 (slow) | −0.06 | −0.21 | −0.36 |
| `f72f591` final fix wave (C1 epilogue) | `m2-f72f591` | 3cc14a9cb | 10 (fast) | −0.06 | −0.21 | −0.71 |
| control: M1 sources rebuilt | `ctl-m1-code` | 758558c2b | 4 (fast) | 0.00 | 0.00 | 0.00 |
| control: M1 logic, decoder +16 B | `ctl-m1-shift16` | ae66914a5 | 10 (fast) | −0.12 | −3.08 | −4.27 |

The boot log's `App version` is taken when the build is configured, so a firmware built just before its commit names the
previous one (`m2-t1`'s a91706eed says `19c1a23-dirty`; the table lists it as the `291405e` firmware on the strength of the
Task 1 build notes, not of its record): the ELF SHA is the identity.

### Memory: int_used and psram_used (M3's cost-axis baseline)
Record `m2-final-all`, both runs of every stream. The two runs agree exactly, and so do the same streams in `m2-t3` (index 4,
a pre-commit build) and the search streams in `m2-t3-final` (index 4): the figures do not change with run, boot or build.
Streams are the search set first, then the held-out set. `refs` is the SPS `max_num_ref_frames`, read from each stream. The
JVT QCIF streams are correctness-only (M1 ruling R9): their fps are listed, not for aggregates or for 30 fps headroom
arguments. The held-out rows have no same-class M1 baseline (`m1-smoke` is fast class), so they carry no Δ.

| stream | size | refs | int_used B | psram_used B | fps run 0 / run 1 (index 5) |
|---|---|---|---|---|---|
| aux_card | 800x480 | 1 | 13,544 | 1,507,340 | 17.11 / 17.11 |
| aux_map | 800x480 | 1 | 13,544 | 1,507,340 | 9.67 / 9.67 |
| full_map | 800x480 | 1 | 13,544 | 1,507,340 | 5.58 / 5.58 |
| aux_map_dark | 800x480 | 1 | 13,544 | 1,507,340 | 9.27 / 9.27 |
| test_640x360 | 640x360 (coded 640x368) | 3 | 12,224 | 1,642,516 | 12.99 / 12.99 |
| BA1_Sony_D | 176x144 | 1 | 6,964 | 99,340 | 56.54 / 56.56 |
| BA2_Sony_F | 176x144 | 5 | 7,016 | 255,004 | 58.35 / 58.35 |
| BANM_MW_D | 176x144 | 1 | 6,964 | 99,340 | 62.54 / 62.54 |
| BA_MW_D | 176x144 | 4 | 7,000 | 216,088 | 60.27 / 60.27 |
| MIDR_MW_D | 176x144 | 4 | 7,000 | 216,088 | 60.29 / 60.29 |
| NRF_MW_E | 176x144 | 3 | 6,984 | 177,172 | 59.15 / 59.15 |

At the target 800x480 stream (1 reference frame): 13,544 B internal and 1,507,340 B PSRAM. The PSRAM is the per-macroblock
state (1500 MBs x 216 B = 324,000 B requested, 327,684 B taken) plus 2 frames (576,047 B requested, 589,828 B taken each).
IDF's TLSF allocator rounds every block up to its size class (+4 B header), so the measured figure is 2.1 % above the sum of
the structure sizes (1,476,094 B), and each extra reference frame at 800x480 costs 589,828 B. The internal figure is 1.0 %
above its sum (13,544 B against 13,404 B). The per-allocation sizes come from a scratch log of every allocation's change in
free heap (M2 Task 3), not from a committed tool.

### Static footprint of `libvideojet.a`
`$IDF_PY -m esp_idf_size --archives bench/build/videojet_bench.map` on the committed firmware's map (the ELF the board ran;
`--archive_details libvideojet.a` lists nothing in `.dram0.data` or `.dram0.bss`):

| flash code | flash rodata | IRAM | DRAM .data / .bss | flash total |
|---|---|---|---|---|
| 50,981 B | 7,090 B | 0 B | 0 B / 0 B | 58,071 B |

The archive holds the wrapper and the vendored decoder. It keeps no static RAM: its code runs from flash through the I-cache,
and its persistent state is all heap, counted in `int_used` and `psram_used` above. The stack its calls use on the caller's
task is not measured by any axis here (the bench task has 16 KiB, `bench/main/bench_main.c`). The whole image is 258,798 B.

### Cost axes for M3 (800x480)
| axis | M2 value |
|---|---|
| internal RAM, decoder heap (`int_used`) | 13,544 B |
| internal RAM, static (IRAM + DRAM of `libvideojet.a`) | 0 B |
| internal RAM, stack of the calling task | not measured (the bench task has 16 KiB) |
| PSRAM (`psram_used`) | 1,507,340 B |
| flash (`libvideojet.a` code + rodata) | 58,071 B |

No axis counts the stack, so a candidate that moves state from the heap to the stack would look like an internal-RAM win: M3
has to measure the decode task's stack high-water mark per stream (`docs/superpowers/m1-carryover.md`).

The spec's library budget is ≤ 100 KB of internal RAM. M3 step 0 would move libvideojet's `.text` into IRAM, which adds 50,981 B
of internal RAM: 13,544 B + 50,981 B = 64,525 B, under 100 KB whether KB is 1000 or 1024 B, excluding the unmeasured stack.
(M3 step 0 below measured it: 50,831 B of IRAM, 64,375 B with the heap.)

## M3 step 0 (decoder code in IRAM, 64 KB / 64 B D-cache)
M3's first task was the "blocking" item of the Noise section: pin where the decoder's code runs, and take what the cache
configuration gives. No decoder logic changed. Three edits:
- `components/videojet/linker.lf` (new), registered with `LDFRAGMENTS` in the component's `CMakeLists.txt`, and the Kconfig switch
  `CONFIG_VJ_IRAM` (default y): with it every `noflash_text` section of `libvideojet.a`, that is all of the library's code, is
  linked into IRAM; with `VJ_IRAM=n` it stays in flash (`* (default)`).
- `bench/sdkconfig.defaults`: `CONFIG_ESP32S3_DATA_CACHE_64KB=y` and `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`. The I-cache stays 16 KB.
- The consumer README gains a "Recommended config" section; CLAUDE.md gains the flash trap (`idf.py -B dir flash` rebuilds).

The firmware is the step-0 tree, built fresh (`-DSDKCONFIG=bench/build/sdkconfig`) and flashed with `esptool write_flash
@flash_args`: ELF SHA256 `e8596983135d…`. Its boot log's `App version` reads `30da81f-dirty`, because that field is taken when the
build is configured, before the commit: the ELF SHA is the identity. Checked before the board ran:
- Placement, `xtensa-esp32s3-elf-nm`: `h264bsdDecode` 0x4038249c, `h264bsdFilterPicture` 0x4038687c, `vj_decode_au` 0x403821f8. All in
  IRAM (0x4037xxxx–0x403Dxxxx), none in flash (0x42xxxxxx).
- `sdkconfig`: `CONFIG_ESP32S3_DATA_CACHE_64KB=y`, `CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`, `CONFIG_VJ_IRAM=y`.
- Host gate, `make -C host check`: `test_api: all passed`, 11 of 11 streams PASS (the host build does not see the change).
- The other branches build, and were not flashed: with `CONFIG_VJ_IRAM=n` (an overlay, in a scratch dir) the same three symbols sit at
  0x42008f10 / 0x4200d368 / 0x42008c6c and `libvideojet.a` has 0 B of IRAM; the `sdkconfig.profile` build links with the code in
  IRAM (51,013 B of IRAM, 32 B of DRAM).

### Spike (what the config is built on)
The spike (branch `spike/step0`, a throwaway worktree) measured each lever alone. Its records, `spike-e0` to `spike-e14` (there is no
`spike-e13`) and `spike-best-prof`, are in that worktree's gitignored `results/bench.jsonl`: they are not versioned, and the new baseline
below reproduces the adopted row. Slow class (idx 5) throughout; Δ against M2 in the same class (`m2-final-all`, exact 17.1052 / 9.6680 /
5.5758 fps), fps as printed to 2 decimals:

| config | record | aux_card / aux_map / full_map fps | Δ % | int_min B |
|---|---|---|---|---|
| M2: code in flash, 16 KB I-cache, 32 KB D-cache with 32 B lines | `m2-final-all` | 17.11 / 9.67 / 5.58 | n/a | 354,639 |
| libvideojet code in IRAM | `spike-e1` | 17.30 / 10.77 / 6.72 | +1.1 / +11.4 / +20.6 | 303,695 |
| code in flash, 32 KB I-cache | `spike-e4` | 17.28 / 10.69 / 6.68 | +1.0 / +10.6 / +19.9 | 338,255 |
| **IRAM + D-cache 64 KB / 64 B lines (adopted)** | `spike-e6` | **22.21 / 12.72 / 7.68** | **+29.8 / +31.5 / +37.8** | 271,699 |
| code in flash, 32 KB I-cache + D-cache 64 KB / 64 B | `spike-e7` (its idx-5 record) | 22.17 / 12.60 / 7.63 | +29.6 / +30.4 / +36.8 | 306,259 |
| IRAM + D-cache 32 KB / 64 B lines | `spike-e9` | 20.64 / 12.00 / 7.21 | +20.7 / +24.1 / +29.3 | 303,695 |

- The 64 B D-cache line is the largest lever and costs no RAM: `spike-e9` against `spike-e1` (IRAM, 32 KB D-cache) is +19.3 / +11.4 / +7.2 %. The
  doubled D-cache size alone (`spike-e10`: 64 KB, 32 B lines) gave +4.7 / +4.0 / +4.7 % over `spike-e1`; on top of 64 B lines (`spike-e6`
  against `spike-e9`) it gives +7.6 / +6.0 / +6.6 % for 31,996 B of heap (303,695 − 271,699 B of `int_min`).
- All of libvideojet fit in IRAM without a linker error: 50,837 B on the spike's sources, 50,831 B in the step-0 build (the spike branched
  before `f72f591`).
- Code in flash with a 32 KB I-cache lands within 1 % of IRAM (`spike-e7` against `spike-e6`: −0.17 / −0.89 / −0.68 %) for 16 KB of RAM instead
  of 51 KB; the library defaults to IRAM because it removes the placement noise (next section).
- The M2 row prints 17.11 aux_card; the plan's table printed 17.12, the M1 baseline's value. The Δ column is computed from exact fps.
- The profile at the adopted config (`spike-best-prof`, a `VJ_PROFILE` build, µs per frame) is in the M3 plan, "Step 0 facts".

### Noise
- Same binary, same class: the 7 `m3-base-*` boots below, exact fps from `total_us`. The boot means of the 5 boots at idx 5 differ by at most
  0.0019 / 0.0011 / 0.0007 % (aux_card / aux_map / full_map), those of the 2 boots at idx 10 by at most 0.0008 / 0.0005 / 0.0002 %, and run 0
  against run 1 of one boot by at most 0.0001 %. All of it is under the 0.004 % the acceptance bar assumes.
- Speed class: idx 10 over idx 5 with the same ELF is +0.510 / +0.336 / +0.231 % (M1's gap was 0.70 / 0.49 / 0.36 %). Compare within a
  class only. The 7 boots drew idx 5 five times, idx 10 twice and idx 4 never.
- Layout, with the code in IRAM (spike, idx 5, Δ against the unperturbed record, aux_card / aux_map / full_map):
  - `spike-e3` against `spike-e1`: a 300-nop function linked ahead of the library moves every libvideojet symbol by +0x26c:
    −0.0003 / −0.0002 / −0.0001 %.
  - `spike-e2` against `spike-e1`: `-falign-functions=64` on the component: −0.02 / −0.09 / −0.09 %.
  - `spike-e12` and `spike-e11b` against `spike-e6`, the adopted config under the layout variants of spike commit `6c966d1`:
    −0.0014 / −0.0007 / −0.0006 % and −0.02 / −0.11 / −0.11 %.
  - So with the code in IRAM **layout noise is at most 0.11 %**, where M2's code in flash moved between −4.3 % and +1.25 %. The profile
    build (`spike-best-prof`: counters and layout together) is −0.31 / −0.20 / −0.15 % against `spike-e6`.
- M3's acceptance bar (plan, Global Constraints), from exact fps against the same-class baseline below: every search stream at least −0.3 %
  and one at least +0.3 %. That is 2.7 times the worst layout noise and over 150 times the measured same-binary noise (0.0019 %).

### New baseline
Records `m3-base-1` to `m3-base-7`: search set, 6 rows each, every hash passes, ELF `e85969831…`, versioned unmodified in
`docs/baseline-m3.jsonl` (the file `tools/compare.py --baseline` takes, and the one M3's scorer reads). The stop rule was two passing boots per
speed class or 10 boots: it took 7 to draw idx 10 twice. fps is exact, frames·10⁶/total_us, averaged over the class's rows. `int_min`,
`int_used` and `psram_used` are the same in every row of every boot.

| class | records | aux_card fps | aux_map fps | full_map fps | ms/frame (aux_card / aux_map / full_map) | int_min B | int_used B | psram_used B |
|---|---|---|---|---|---|---|---|---|
| slow (idx 5) | `m3-base-1`, `-2`, `-3`, `-5`, `-6` | 22.2074 | 12.7163 | 7.6810 | 45.03 / 78.64 / 130.19 | 271,699 | 13,544 | 1,507,340 |
| fast (idx 10) | `m3-base-4`, `-7` | 22.3206 | 12.7590 | 7.6987 | 44.80 / 78.38 / 129.89 | 271,699 | 13,544 | 1,507,340 |

- Against M2 in the same class: slow +29.83 / +31.53 / +37.75 % (`m2-final-all`), fast +29.57 / +31.34 / +37.59 % (`m2-t3-final`, idx 4).
- Against the spike: the slow class is `spike-e6` within 0.0006 %, a rebuild one commit later. The fast class, 22.3206 / 12.7590 / 7.6987, is
  +0.02 / +0.11 / +0.11 % over `spike-e11` (idx 10, one of the layout variants above), the same band as the layout noise.
- No idx 4 boot was drawn; indexes 4 and 10 are one speed class (M1, Noise above), and `tools/compare.py` treats them so.
- Sanity check, `python3 tools/compare.py m3-base-1 --baseline docs/baseline-m3.jsonl --tol 0.3` (printed 2-decimal fps; not a gate):
  ```
  aux_card                    22.21 vs    22.21 fps (slow)  -0.00 %  ok
  aux_map                     12.72 vs    12.72 fps (slow)  +0.00 %  ok
  full_map                     7.68 vs     7.68 fps (slow)  -0.00 %  ok
  PASS
  ```

### Cost axes after step 0 (800x480)
`$IDF_PY -m esp_idf_size --archives bench/build/videojet_bench.map` on the step-0 ELF; the M2 column is the table in M2 above.

| axis | M2 | step 0 |
|---|---|---|
| internal RAM, decoder heap (`int_used`) | 13,544 B | 13,544 B |
| internal RAM, static: IRAM / DRAM of `libvideojet.a` | 0 B / 0 B | 50,831 B / 0 B |
| **library internal RAM (heap + IRAM + DRAM; plan budget ≤ 100 KB)** | 13,544 B | **64,375 B** (13,544 + 50,831) |
| internal RAM, stack of the calling task | not measured | not measured |
| PSRAM (`psram_used`) | 1,507,340 B | 1,507,340 B |
| flash, `libvideojet.a` code + rodata | 58,071 B | 7,090 B of rodata; the code is stored in the image and copied to IRAM at boot |
| `int_min` (bench, boot-wide) | 354,639 B | 271,699 B |
| D-cache (the consumer's config, reported apart from the library) | 32 KB, 32 B lines | 64 KB, 64 B lines |

- `int_min` fell by 82,940 B: 50,944 B for the IRAM code (the heap starts that much higher: `.dram0.dummy` 41,984 B → 92,928 B between a
  `VJ_IRAM=n` and a `VJ_IRAM=y` build, and `spike-e0` → `spike-e1` 354,639 → 303,695 B) and 31,996 B for the D-cache (`spike-e9` → `spike-e6`).
  Three measures of the IRAM cost agree to within 149 B: the archive's own 50,831 B, `.iram0.text` growing 50,980 B, and the heap losing
  50,944 B. The last is what the library costs the heap; the differences were not broken down.
- The whole image is 258,666 B (M2: 258,798 B). The stack is still unmeasured by any axis.

## M3 frontier
Each candidate is scored with `tools/score.sh` (fresh build, slow class pinned with `--tuning 5`, exact fps =
frames·10⁶/total_us, run 0). Δ is against the row above. A KEEP merges as a fast-forward, and its own record becomes
`docs/baseline-m3.jsonl` (ruling R8). Records live in `results/bench.jsonl` of the checkout that ran them; the last two
are copied into `.superpowers/sdd/2026-10-10-videojet-m3-speed/baseline-t{3,6c}.jsonl`.

| candidate | record | idx | ELF | aux_card / aux_map / full_map fps | Δ % | int_used B |
|---|---|---|---|---|---|---|
| step 0 (d4d219f) | `m3-base-1` | 5 | e85969831 | 22.2075 / 12.7164 / 7.6810 | | 13,544 |
| 5a skip copy (c437933) | `m3-base-skipcopy-2` | 5 | 18c888b1c | 23.1732 / 12.9092 / 7.6949 | +4.35 / +1.52 / +0.18 | 13,544 |
| T9 reset generation (0abb6d3) | `m3-resetgen2` | 5 | 5b752d531 | 24.3856 / 13.2758 / 7.8423 | +5.23 / +2.84 / +1.92 | 13,544 |
| T3 skip elision (088983c; af919ec adds tests only) | `m3-elide` | 5 | 48a121b56 | 59.8110 / 16.1626 / 7.8575 | +145.27 / +21.75 / +0.19 | 21,136 |
| T6c lazy mbLayer clear (c4fbb31) | `m3-lazyclear` | 5 | e0fa69db4 | 59.9155 / 16.2917 / 7.9250 | +0.17 / +0.80 / +0.86 | 21,136 |
| tables in DRAM, linker `noflash` (9301581) | `m3-rodata` | 5 | c28ae06da | 60.3359 / 16.5266 / 8.0799 | +0.70 / +1.44 / +1.95 | 21,136 |
| T10 deblock records + edge-less skip (bc05b06) | `m3-bsrec` | 5 | bcfa20de0 | 73.0733 / 16.9407 / 8.1067 | +21.11 / +2.51 / +0.33 | 39,572 |
| T6a inlined bit reader, clz Exp-Golomb (0c01292) | `m3-bitreader` | 5 | a98083e73 | 73.8422 / 17.2162 / 8.2398 | +1.05 / +1.63 / +1.64 | 39,572 |
| cold code to flash, memory axis R10 (c0d006f) | `m3-coldflash` | 5 | d23ede2cd | 73.8375 / 17.2127 / 8.2384 | -0.01 / -0.02 / -0.02 | 39,572 |
| MC stage B: C luma kernels, register 6-tap (6f69e98) | `m3-mc-c` | 5 | e2efde94c | 73.8374 / 17.7296 / 8.7834 | -0.00 / +3.00 / +6.62 | 39,572 |
| T6d table-driven CAVLC (8f61f57; 2459a4c adds host/test_cavlc.c) | `m3-cavlctab2` | 5 | 63aeed1ae | 73.8668 / 17.8425 / 8.8237 | +0.04 / +0.64 / +0.46 | 39,572 |
| T11 D-cache autoload over the decoder's span (931adda; docs 3930d59) | `m3-autoload` | 5 | 2b2cd7855 | 80.7704 / 18.7441 / 9.2244 | +9.35 / +5.05 / +4.54 | 39,572 |
| PIE motion compensation, per-width alignment guard (e6fd90d, 7931647, 22bf784; test/mc_pie) | `m3-mc-pie4` | 5 | d77df6d26 | 80.9724 / 20.6899 / 11.2375 | +0.25 / +10.38 / +21.82 | 39,572 |
| cheap deblock record for skip MBs (1dc0cb2) | `m3-skiprec` | 5 | f3ae8657c | 94.9683 / 21.2162 / 11.3033 | +17.28 / +2.54 / +0.59 | 39,572 |
| PIE residual-add/write-out (896bf08; test/wr_pie, host/test_write.c) | `m3-piewrite` | 5 | 198ddf43f | 94.9629 / 21.4728 / 11.4429 | -0.01 / +1.21 / +1.23 | 39,572 |
| PIE 4x4 inverse transform + aligned mbLayer (154fe8a; host/test_idct.c, test/idct_pie) | `m3-pieidct` | 5 | 4eca3af74 | 95.3020 / 21.8882 / 11.6485 | +0.36 / +1.93 / +1.80 | 39,580 |

Cumulative since M2 (slow class 17.11 / 9.67 / 5.58), at the T10 tip: **+327 % / +75 % / +45 %**. Skip elision elides 1,438 / 797 / 8.4 MBs
per frame (aux_card / aux_map / full_map), against the 1,437 / 800 / 6 that the earlier cost model predicted. aux_card is at the
spec's 60 fps decode-only goal; aux_map, the ≥30 fps gate for the Android Auto consumer, is next.

Library internal RAM at the T10 tip: heap 39,572 B (+18,000 B of deblock records) + IRAM 51,713 B + DRAM 7,090 B = 98,375 B
of the 100 KB budget, stack excluded. An internal-RAM row ring for `mbStorage_t` (T12, ~22 KB) no longer fits: T12 becomes a
compaction of `mbStorage_t` in PSRAM, or the records move to PSRAM (one line, bit-exact either way).

At the cold-code tip: library internal RAM = heap 39,572 + IRAM 43,843 + DRAM 7,058 = 90,473 B (IRAM −7,798 B for concealment and SPS/PPS/VUI parsing, which now run from flash: once per parameter-set NAL or error). Cumulative since M2 (17.11 / 9.67 / 5.58): +332 % / +78 % / +48 %.

### Profile split of `inter` (T7 Stage A, no candidate)
Sub-counters inside `inter`, compiled out by default (`vj_prof.h`; the timed build's libvideojet objects are byte-identical to
6f8a03b's, every section of all 29). Profile build `m3-mc-prof-a` at 6f8a03b + the counters: ELF 625cde6bd, slow class (idx 5),
PASS, both runs within 1 µs. µs per frame; MC = `h264bsdPredictSamples` (luma + chroma), write = `h264bsdWriteOutputBlocks` /
`h264bsdWriteMacroblock`, copy = the direct skip copy (5a), rest = inter minus the four (elision checks, dispatch, counter overhead).

| stream | frame | inter | MV pred | copy | MC: luma + chroma | write | rest |
|---|---|---|---|---|---|---|---|
| aux_card | 13,934 | 4,273 | 1,837 | 1,083 | 299: 159 + 136 | 264 | 790 |
| aux_map | 59,426 | 21,034 | 2,104 | 2,461 | 12,077: 8,659 + 3,314 | 3,619 | 773 |
| full_map | 124,019 | 59,096 | 3,060 | 2,424 | 42,588: 32,022 + 10,275 | 10,172 | 852 |

MC is the bulk of `inter` on the moving streams (57 % aux_map, 72 % full_map); write-out is next (17 %). On aux_card, MV
prediction of the static skips is the largest part (43 %). Luma by quarter-sample position, with calls and pixels per frame from
a host count (`-Wl,--wrap=h264bsdPredictSamples`, deterministic, so the board ran the same calls); cycles at 240 MHz:

| position | full_map µs | calls | px | cyc/px | aux_map µs | cyc/px |
|---|---|---|---|---|---|---|
| G (full-sample copy) | 2,956 | 520 | 101,484 | 7.0 | 1,234 | 7.5 |
| d / h / n (vertical) | 4,089 / 2,463 / 2,922 | 158 / 118 / 119 | 29,784 / 21,998 / 21,341 | 33.0 / 26.9 / 32.9 | 1,353 / 708 / 950 | 33.4 / 27.3 / 33.6 |
| a / b / c (horizontal) | 3,007 / 1,769 / 2,624 | 138 / 99 / 121 | 25,973 / 18,554 / 22,490 | 27.8 / 22.9 / 28.0 | 989 / 437 / 863 | 28.3 / 23.4 / 28.6 |
| e / g / p / r (diagonal) | 870 / 730 / 642 / 812 | 24 / 20 / 19 / 23 | 4,132 / 3,451 / 3,097 / 3,901 | 50.5 / 50.8 / 49.8 / 49.9 | 557 together | 51 |
| f / q / i / k (centre quarter) | 1,999 / 1,870 / 1,834 / 2,036 | 31 / 29 / 29 / 32 | 5,177 / 4,850 / 4,906 / 5,463 | 92.7 / 92.6 / 89.7 / 89.5 | 1,389 together | 93–96 |
| j (centre half) | 1,400 | 26 | 4,366 | 77.0 | 181 | 79.7 |
| chroma (all 4 kinds) | 10,275 | 1,507 | 140,483 | 17.6 | 3,314 | 17.8 |

- Ranking by µs (full_map / aux_map): chroma 10.3 / 3.3 ms, vertical 9.5 / 3.0, horizontal 7.4 / 2.3, centre quarter 7.7 / 1.4,
  full-sample copy 3.0 / 1.2, diagonal 3.1 / 0.6, j 1.4 / 0.2. Quarter positions outnumber half positions 3-3.5 : 1 (calls:
  aux_map 73k : 21k, full_map 223k : 73k).
- The centre kernels run 3x slower per pixel than the 1D ones: their 1.9 KB stack frame (`p1[]` + `table[]`) puts GCC's spill
  slots beyond `l32i`'s 1020 B reach, so every spill costs `movi` + `add` + `l32i` (seen in the disassembly).
- Partitions: the x264 streams use 16x16 / 16x8 / 8x16 / 8x8 only; the JVT held-out streams cover 8x4 / 4x8 / 4x4 and every position.
  Blocks that cross the picture edge (the FillBlock path): 3.7 % of full_map's luma calls.

### D-cache autoload (T11): hardware rules and trap evidence
Measured on the bench board on 2026-10-11 with two scratch apps (kept in the session scratchpad, not in the repo; the comment in
`components/videojet/src/videojet.c` summarises these rules): a probe that masks the cache-error interrupt and reads
`EXTMEM_CACHE_ILG_INT_ST` instead of panicking, and a re-arm test that drives the library through its public API on streams from
the `streams` partition. In the probe a line counts as cached when a timed load of it, with autoload switched off again, takes under
60 cycles. 64 KB / 64 B D-cache. `EXTMEM_DCACHE_AUTOLOAD_CTRL`: bit 0 SCT0_ENA, bit 2 ENA, bit 3 DONE, bit 4 ORDER, bits 6:5 RQST,
bits 8:7 SIZE. The feature's own score is record `m3-autoload`, slow class, at the T6d tip: 73.87 / 17.84 / 8.82 to 80.77 / 18.74 / 9.22 fps
(+9.35 / +5.05 / +4.54 %).

ROM (`esp32s3_rev0_rom.elf`, disassembled): `Cache_Enable_DCache_Autoload` is `ctrl |= BIT(2)`. `Cache_Config_DCache_Autoload` switches
autoload off first (it waits for DONE) and writes only ORDER, RQST and SIZE; its `ena` means "apply these". `Cache_Config_DCache_Region_Autoload`
checks `region < 2` and that `addr` and `addr + size - 1` both pass `Cache_Address_Through_DCache` (inside the 32 MB data window
0x3C000000..0x3DFFFFFF), clears the autoload buffer if the section was on, then stores `addr` and `size` (27 bit) verbatim and sets
SCT0_ENA. It rounds nothing. Its check lets an end of exactly 0x3E000000 through (the last byte, 0x3DFFFFFF, is inside the window);
the hardware refuses it at the enable.

**What the hardware accepts** (ILG status read after `Cache_Enable_DCache_Autoload()`):

| region | result |
|---|---|
| start 1 or 33 B past a line, size 0x1000; size 0x1001; both unaligned; size 1 line; size 1 B | accepted (`rc=0`, no ILG), registers read back verbatim |
| **end = 0x3E000000**: `[0x3DFF0000, +0x10000)`, `[0x3DFF0001, +0xFFFF)`, `[0x3C030000, +0x1FD0000)`, autoload sizes 0, 1 and 3 | **ILG bit 3 set at the enable**: "Dcache preload parameter configuration error", a panic without the mask |
| end = 0x3E000000 minus 1, 2, 4, 16, 32, 63 or 64 B (size 3); minus 64, 128, 256 B, 1 KB or 4 KB (sizes 0, 1 and 3) | accepted |
| end = the end of the 8 MB mapping (0x3C830000); the whole mapping | accepted |

**What one miss fetches** (a 16 KB region, miss trigger; "cached" = a timed load hits afterwards):

| case | result |
|---|---|
| ascending, size 0 / 1 / 2 / 3 | the next 1 / 2 / 3 / 4 lines |
| descending, size 0 / 3 | the previous 1 / 4 lines |
| the miss is on the last line of the region | nothing |
| the miss is on the 2nd / 3rd / 4th last line (size 3) | 1 / 2 / 3 lines: the request is cut at the region end |
| aligned start, miss on the line just below the region | the first 1 / 4 lines of the region (size 0 / 3) |
| start 32 B past a line boundary, miss on the line below that boundary | nothing: the whole request is dropped |
| start 32 B past a line boundary, miss on that first line | the next 4 lines (size 3) |
| end 32 B inside a line, miss on the line before it | the half-covered line is fetched, the next is not |
| region ending at, or 64 KB past, the end of the mapping; miss on the last mapped line (sizes 1, 3) | no ILG, no ACS fault |

So the start is rounded down and the end up to a line, and the end is kept below 0x3E000000 (the library uses one line below).

**Trap 2 and the re-arm** (re-arm test on the final code, `CONFIG_VJ_DCACHE_AUTOLOAD_SIZE=1`, `ctrl` read after each step):

| step | ctrl | ENA | region (SCT0) |
|---|---|---|---|
| boot, after reading the stream clips | 0x008 | 0 | none |
| after the first AU (SPS + PPS + IDR: the blocks are allocated inside that call) | 0x004 | 1 | none |
| after the second AU | 0x085 | 1 | 0x3C07A740 + 0x16CA40 |
| after `esp_partition_read()` | **0x089** | **0** | unchanged |
| after the next `vj_decode_au()` | **0x085** | **1** | unchanged |
| after `esp_flash_read()`, then after the next call | 0x089, 0x085 | 0, 1 | unchanged |
| after another `esp_partition_read()`, then `vj_decode_au(NULL, ...)` (VJ_ERR_ARG) | 0x089 | 0 | the read cleared it; an argument error arms nothing |
| someone else's setting (descending, both triggers, size 3, another region), then one call | 0x1D5, 0x085 | 1, 1 | replaced by the right one |

At 800x480 the blocks were `mb` 0x3C07A750 (+324,000 B) and frames 0x3C0CA754 and 0x3C15A758 (+576,000 B each): region
0x3C07A740 .. 0x3C1E7180 = 1,493,568 B, inside the mapping 0x3C030000 .. 0x3C830000 (the heap's own PSRAM use there, 1,507,340 B, counts
each big block rounded up to a 16 KB step). An SPS change to 640x360 (dpbSize 1 to 3): the changing call still ran on the old region, the
next call programmed 0x3C07A740 + 0x18F440 (1,635,392 B); a second decoder got its own region. 40 + 12 + 2 frames, with flash operations
and the SPS change in between, all matched their golden hashes. 25 checks, 0 fails.

**Interactions with IDF, read from the 5.4.2 sources, not tested:**
- `esp_cache_msync()` C2M reaches `Cache_WriteBack_Addr()` (`hal/esp32s3/include/hal/cache_ll.h:377`), which on the S3 is the patch in
  `esp_rom/patches/esp_rom_cache_esp32s2_esp32s3.c` (the erratum `ESP_ROM_HAS_CACHE_WRITEBACK_BUG`, `esp_rom_caps.h:32`). It wraps the ROM
  write-back in `Cache_Suspend_DCache_Autoload()` / `Cache_Resume_DCache_Autoload()` (lines 148-151, "avoid load cachelines being written
  back"). A `vj_decode_au()` on another core enables autoload inside that window.
- esp_lcd's RGB panel calls `Cache_Start_DCache_Preload()` from its bounce-buffer ISR when the framebuffer is behind the cache
  (`esp_lcd_panel_rgb.c:841-845`). The ROM function suspends autoload first (disassembly), and no IDF code calls `Cache_End_DCache_Preload()`.
- Flash operations: `esp_cache_suspend_ext_mem_cache()` (`esp_mm/esp_cache.c:159`) calls `cache_hal_suspend()`, which drops the old autoload
  state, and `cache_hal_resume()` restores only the boot-time flag (off): that is the ENA clear above.

### PIE motion compensation (T8 Stage C)
`CONFIG_VJ_PIE` kernels (`src/vj_mc_pie.S`) for 8 and 16 wide partitions whose window lies inside the picture: all 16 luma
positions (1D 6-tap rows/columns, diagonal = horizontal then vertical-averaged, centre = 16-bit two-pass j then a 1D
average) and both chroma planes.
- Bit-exactness, on the board: the IDF app `test/mc_pie` links the library's PIE `h264bsdPredictSamples`
  and a second copy of `h264bsd_reconstruct.c` built with PIE off, and compares their 384-byte outputs over host/test_mc.c's
  whole case space (8,314,432 cases) plus a 6x4-MB picture where most windows are inside (7,379,984): **15,694,416 cases,
  0 mismatches**, and 115,312 sampled cases against the standard's equations, 0 mismatches. Any change to
  `vj_mc_pie.S` (or to the C kernels it mirrors) must re-run it on the board (`test/mc_pie/README.md`).
- Cycles per `h264bsdPredictSamples` call (luma + chroma), 16x16, reference in internal RAM (compute only), C → PIE:
  G 2,654 → 628; d/h/n 7,269 / 6,498 / 7,267 → 1,165 / 1,082 / 1,172; a/b/c 7,163 / 6,380 / 7,167 → 1,088 / 989 / 1,095;
  e/g/p/r ~12,210 → ~1,710; f/q 12,874 / 12,926 → 2,828 / 2,812; i/k 13,520 / 13,562 → 2,917 / 2,922; j 11,223 → 2,266.
- In the decoder (profile build at 6f69e98 + PIE, slow class), MC per frame: full_map 34,935 → 15,615 µs (luma 24,487 →
  9,762, chroma 10,283 → 5,689), aux_map 10,341 → 5,395 µs. Luma now runs at 5–11 cycles per pixel and chroma at ~10, of
  which compute is ~2–4: the rest is reference-line misses, a preload candidate (T11), not a kernel one.

## Deblock worker (VJ_DEBLOCK_WORKER), fix round 1 (budget)
Bench config, 800x480, tuning 5, ELF 12f40b963 (worker on) / 453b... off build same tree. Static from `esp_idf_size --archives`:
libvideojet.a on: DRAM 7,486 + 8 (.bss) + IRAM 49,034 = 56,528 B (off: 7,486 + 46,430 = 53,916). Heap `int_used`: 42,300 on, 39,572 off.
Total internal: 56,528 + 42,300 = **98,828 B** (off 93,488). Marks (n + height = 1,530 B) and the 4-bank undo log are PSRAM-first (`vj_alloc_psram`);
worker stack 2,048 B (988 B used at 3,072 in the -O2 bench build); PSRAM 1,625,620 vs 1,507,340 (+118,280).
fps off -> on: 95.30 / 21.89 / 11.65 -> 100.95 / 26.03 / 13.96 (3 boots, `--set all`, PASS).
