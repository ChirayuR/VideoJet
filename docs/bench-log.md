# Bench log

fps = frames / total decode time, decode-only. Board: Waveshare 3.49 V2, S3 240 MHz, octal PSRAM 80 MHz.
Correctness floor: every hash passes. Search set only. Each value is the mean over the run's rows
(run 0 and run 1 of a boot), then over the boots at that tuning index.
Source: `results/bench.jsonl` (gitignored), records `baseline-r1` to `baseline-r8`: 8 boots, each `tools/bench.py
--set search`, all PASS (6 rows each), each record carries its `psram_tuning`. The records this log cites
(`baseline-r1` to `baseline-r8`, `baseline-prof`, `timed-restore-check`) are versioned, unmodified, in
`docs/baseline-m1.jsonl`. They predate the `elf_sha` and `app_version` fields, so the binaries are named below, from the
boot logs.
"From row times" below means fps recomputed as frames·10⁶ / (idr_us + p_mean_us·(frames−1)). The firmware prints fps
with 2 decimals only, and p_mean_us is the mean truncated to a whole µs. The formula reproduces the printed fps on 98 of
the 100 rows in `bench.jsonl`: all 84 search-set rows (so all 60 rows of the versioned records) and 14 of the 16
held-out rows of the `m1-smoke` record. The two misses are that record's BA_MW_D and MIDR_MW_D run 1 (printed 60.41 and
60.44, formula 60.42 and 60.45: the truncation).

## Noise
- within one boot: max |run0 − run1| fps per stream = 0.00 for aux_map, aux_card and full_map (printed fps, 8 boots).
  From row times: 0.0001 aux_map, 0.0003 aux_card, 0.00003 full_map (at most 0.0017 %).
- across boots: PSRAM tuning index observed = {5, 10}; per-index mean fps per stream below.
  - Within an index the boots agree: the 7 boots at index 5 all print 17.12 / 9.69 / 5.60 (aux_card / aux_map / full_map),
    and from row times the boot means differ by at most 0.00015 fps (aux_card; 0.0022 % at worst, on full_map).
  - Between indexes: spread = 0.70 % aux_card, 0.49 % aux_map, 0.36 % full_map, from row times (index 10 faster than 5).
    The printed fps give 0.70 / 0.41 / 0.36 %, rounding included.
  - So the index shifts fps by 0.4–0.7 %, and within an index the noise is at most 0.0022 %: over a hundred times smaller.
- index frequency: 7 of the 8 baseline boots landed on 5, one (r5) on 10. Over the 10 records in `docs/baseline-m1.jsonl`
  (those 8, `baseline-prof` and `timed-restore-check`) the index was 5 ×7, 10 ×2, 4 ×1.
  Index 4 has no row below, because the 8 baseline boots did not draw it. Record `timed-restore-check` (the timed
  firmware rebuilt after the profile build, tuning 4) printed 17.24 / 9.73 / 5.62 (aux_card / aux_map / full_map), the same
  as the index 10 row (within 0.002 % from row times). So 4 and 10 form one fast speed class, and 5 is the slower, more
  common one. The `m1-fix1*` records predate the `psram_tuning` field.
- code layout: a change of code layout alone moves fps by about ±0.5 %. The profile firmware (`baseline-prof`) and the
  timed one (`baseline-r5`) both booted at index 10 and do the same decoder work, yet from row times the profile firmware
  is −0.43 % on aux_card, +0.22 % on aux_map and +0.50 % on full_map (printed fps: −0.46 / +0.31 / +0.53 %). That mixed-sign
  difference comes from code placement (and the counters), not from the speed class. So the candidate-to-candidate noise
  floor is about ±0.5 %, not the 0.0022 % above: that figure is same-binary, same-speed-class noise, i.e. how well one
  binary repeats.
- rule for M3: compare candidates A/B/B/A across resets, and only within the same tuning index (or report both).
  An A/B pair from boots at different indexes can fake a 0.4–0.7 % gain or loss. `bench.py` cannot choose the index:
  every reset draws it afresh, so a boot at the odd index has to be re-run, or compared only with its own index.
- acceptance rules:
  - M3 accepts a win only at ≥ 1 % over the baseline in the same speed class, or after it survives a layout perturbation
    (the same candidate relinked with `-falign-functions=64`, for example).
  - M2's "fps unchanged" means the same speed class and |Δ| ≤ 0.6 % per stream against `docs/baseline-m1.jsonl`.

## Results
| date | label | tuning | aux_map fps | aux_card fps | full_map fps | aux_map IDR ms / P p95 ms | int_min KB | notes |
|---|---|---|---|---|---|---|---|---|
| 2026-10-10 | baseline (unmodified h264bsd, IDF defaults + -O2) | 5 | 9.69 | 17.12 | 5.60 | 139.36 / 113.69 | 346.3 | 7 boots (r1–r4, r6–r8) |
| 2026-10-10 | baseline (same binary) | 10 | 9.73 | 17.24 | 5.62 | 139.06 / 113.25 | 346.3 | 1 boot (r5) |

int_min is 354,655 B in every row of every boot (KB = 1024 B). Per-stream P p95 and IDR on the other streams:
index 5 aux_card 60.36 / 116.78 ms (P p95 / IDR), full_map 197.31 / 217.46 ms; index 10 aux_card 59.96 / 116.46 ms,
full_map 196.62 / 217.19 ms.
Binary: the Task 4 image, not rebuilt for this log (boot log: app version `88ed2d6-dirty`, compiled `Oct 10 2026 04:13:34`,
ELF SHA256 `c31a1eec3…`).

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
It is flashed with the same `-B`, run with
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
for the timed baseline at index 10 (Results table): within about 0.5 %, mixed sign. That is the size of the code-layout noise
(Noise section), so the counter cost cannot be told apart from it. The timed firmware rebuilt with these changes and flashed
back printed 17.24 / 9.73 / 5.62 at index 4 (record `timed-restore-check`, no `VJP` lines), the same numbers as the index 10
row.

Reading: `inter` (MV prediction + motion compensation + residual add/clip + write-out) dominates all three streams; this
profile does not split motion compensation from the write-out. aux_card is skip-heavy (parse is 0.9 % of the frame, which
suggests almost no macroblock is coded; the counters do not count macroblocks, so that is inferred), and its 72 % `inter` is
mostly the skipped-macroblock copy path (MV prediction, reference copy, `h264bsdWriteMacroblock`); deblock is next at 14 %.
aux_map and full_map spend about half the frame in `inter` (49 % / 48 %); the rest is deblock 17 % / 20 %, parse 14 % / 16 %,
recon minus `inter` (residual dequantisation and inverse transform, intra prediction with its write-out) 13.5 % / 12 %,
and other 6 % / 5 %.
