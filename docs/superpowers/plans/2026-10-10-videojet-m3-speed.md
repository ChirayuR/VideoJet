# VideoJet M3a: Speed Sprint Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Raise VideoJet's bit-exact decode fps on the ESP32-S3. Pin code placement and the cache config (step 0), stand up a one-command scorer, then search the optimisation ladder one mechanism at a time.

**Architecture:** M2's library is unchanged at the API level. Step 0 puts the decoder's code in IRAM through a linker fragment. The bench adopts a 64 KB / 64 B D-cache. Every later task is a meta-harness candidate: one mechanism in `components/videojet/h264bsd/**`, written by a proposer agent, gated on the host (hashes), scored on the board (fps), and kept only if it clears the acceptance bar.

**Tech Stack:** ESP-IDF v5.4.2, xtensa gcc 14.2, C99, h264bsd (vendored), Python 3 tools, the board at /dev/ttyACM0.

**Spec:** `docs/superpowers/specs/2026-10-10-videojet-design.md` (M3 = optimisation ladder + meta-harness framing). Step-0 evidence: the spike in "Step 0 facts" below. Carry-over: `docs/superpowers/m1-carryover.md` ("M3" sections).

## Global Constraints
- **Correctness floor, never traded:**
  - On the host, `make -C host check` passes: 11 streams, every frame hash equals the golden, plus `test_api`.
  - On the board, `bench.py` reports `fail=0` and complete frame counts on every stream it runs.
- **Scope:** decoder only. No display, USB or protocol code (CLAUDE.md). Candidates touch only `components/videojet/**`, plus `bench/sdkconfig.defaults` in Task 1.
- **No stream-specific constants** (resolution, crop, margins, MB counts of the corpus) in decoder code.
- **One mechanism per candidate,** ponytail-minimal. A candidate that needs a second mechanism is two candidates.
- **Library internal-RAM budget ≤ 100 KB:** heap `int_used` + IRAM code + DRAM + any worker stack. The D-cache carve-out is the consumer's config and is reported separately.
- **Public API (`videojet.h`):** additive changes only. `vj_decode_au()` returns with no decode work in flight, worker included.
- **Board protocol:**
  - Build in a fresh build dir with `-DSDKCONFIG=<dir>/sdkconfig`, record the ELF SHA256 prefix, flash with `esptool write_flash @flash_args` from that dir, then run `bench.py --elf <dir>/videojet_bench.elf`.
  - **Never** `idf.py -B <dir> flash`: it rebuilds from the current tree. The spike measured the wrong tree that way.
- **Speed class:** compare only within the same PSRAM tuning class (5 = slow; 4 and 10 = fast).
- **Acceptance bar** (from step 0: layout noise ≤ 0.11 %, same binary ≤ 0.004 %): keep a candidate only if both hold, with fps computed exactly as frames·10⁶/total_us against the same-class M3 baseline:
  - every search stream is ≥ −0.3 %;
  - at least one stream is ≥ +0.3 %.
- **Dual-core candidates** also need:
  - `--set all` PASS;
  - 3 boots with every hash passing, to catch races;
  - a host race check (see Task 4).
- **Promotion:** a kept candidate merges into `m3-speed`. The next candidate branches from the new tip and is scored against a re-baselined tip; Task 2's scorer re-baselines automatically.

## Step 0 facts (spike, 2026-10-10, branch `spike/step0`, records `spike-*`)
Slow class (idx 5); Δ is against M2 in the same class.

| config | aux_card / aux_map / full_map fps | Δ % | int_min B |
|---|---|---|---|
| M2: flash code, I16, D32/32B | 17.12 / 9.67 / 5.58 | — | 354,639 |
| libvideojet text in IRAM | 17.30 / 10.77 / 6.72 | +1.1 / +11.4 / +20.6 | 303,695 |
| flash, I32 | 17.28 / 10.69 / 6.68 | +1.0 / +10.6 / +19.9 | 338,255 |
| **IRAM + D64 KB / 64 B line (adopt)** | **22.21 / 12.72 / 7.68** | **+29.8 / +31.5 / +37.8** | 271,699 |
| flash, I32 + D64/64B | 22.17 / 12.60 / 7.63 | +29.6 / +30.4 / +36.8 | 306,259 |
| IRAM + D32 KB / 64 B line | 20.64 / 12.00 / 7.21 | +20.7 / +24.1 / +29.3 | 303,695 |

- **Layout:** a uniform shift of every libvideojet symbol by +0x26c moves fps ≤ 0.0004 % with the code in IRAM.
- **The 64 B D-cache line** is the largest lever and costs 0 B of RAM.
- **IRAM fit:** all of libvideojet, 50,837 B, fits without a linker error.
- **Profile at the adopted config** (`spike-best-prof`), in µs per frame:

| stream | frame | parse | recon | of which inter | deblock | other |
|---|---|---|---|---|---|---|
| aux_card | 45,170 | 413 (0.9 %) | 32,540 (72.0 %) | 30,366 (67.2 %) | 7,406 (16.4 %) | 4,811 (10.7 %) |
| aux_map | 78,797 | 11,684 (14.8 %) | 46,063 (58.5 %) | 35,803 (45.4 %) | 15,160 (19.2 %) | 5,890 (7.5 %) |
| full_map | 130,384 | 20,571 (15.8 %) | 73,615 (56.5 %) | 59,787 (45.9 %) | 29,410 (22.6 %) | 6,788 (5.2 %) |

---

### Task 1: Step 0 — IRAM placement, D-cache config, M3 baseline

**Files:**
- Create: `components/videojet/linker.lf`
- Modify: `components/videojet/Kconfig` (add `VJ_IRAM`)
- Modify: `components/videojet/CMakeLists.txt` (`LDFRAGMENTS`)
- Modify: `bench/sdkconfig.defaults` (D-cache)
- Modify: `components/videojet/README.md` (recommended consumer config, IRAM cost)
- Create: `docs/baseline-m3.jsonl` (search-set records only, both speed classes)
- Modify: `docs/bench-log.md` (M3 section: the step-0 table above + the new baseline), `CLAUDE.md` (board-loop trap, status)

**Interfaces:**
- Produces: `CONFIG_VJ_IRAM` (bool, default y). Records `m3-base-*` in `docs/baseline-m3.jsonl`; Task 2's scorer reads that file.

- [ ] **Step 1: Linker fragment.** Create `components/videojet/linker.lf`:
```
[mapping:videojet]
archive: libvideojet.a
entries:
    if VJ_IRAM = y:
        * (noflash_text)
    else:
        * (default)
```
- [ ] **Step 2: Kconfig.** Append to `components/videojet/Kconfig`, inside the existing menu:
```
    config VJ_IRAM
        bool "Run the decoder from IRAM"
        default y
        help
            Puts libvideojet's code (about 51 KB) in internal RAM. On the bench it gained +1 / +11 / +21 %
            fps (aux_card / aux_map / full_map) over flash code with the default 16 KB I-cache, and it makes
            fps independent of code layout. Off: a 32 KB I-cache gets within 1 % for 16 KB of RAM.
```
- [ ] **Step 3: Register the fragment.** In `components/videojet/CMakeLists.txt`, add `LDFRAGMENTS "linker.lf"` to `idf_component_register(...)`.
- [ ] **Step 4: Bench config.** Append to `bench/sdkconfig.defaults`:
```
CONFIG_ESP32S3_DATA_CACHE_64KB=y
CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y
```
- [ ] **Step 5: Build fresh and check placement.**
  - Run `rm -rf bench/build && idf.py -C bench -B "$PWD/bench/build" -DSDKCONFIG="$PWD/bench/build/sdkconfig" build`.
  - Then run `xtensa-esp32s3-elf-nm bench/build/videojet_bench.elf | grep -E ' T (h264bsdDecode|h264bsdFilterPicture|vj_decode_au)$'`.
  - Expected: every address is in 0x4037xxxx–0x403Dxxxx (IRAM), none in 0x42xxxxxx.
  - Also expect `grep -E 'CONFIG_ESP32S3_DATA_CACHE_(64KB|LINE_64B)=y' bench/build/sdkconfig` to print both lines.
- [ ] **Step 6: Host gate.** Run `make -C host check`. Expected: `test_api: all passed`, 11/11 PASS (no host change, so this only confirms).
- [ ] **Step 7: Baseline boots.**
  - Flash with `cd bench/build && $IDF_PY -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash @flash_args`.
  - Run `$IDF_PY tools/bench.py --label m3-base-N --set search` for N = 1, 2, …, resetting between runs. Stop when there are ≥ 2 passing records in the slow class (idx 5) and ≥ 2 in the fast class (idx 4 or 10), or after 10 boots.
  - Expected fps: slow ≈ 22.21 / 12.72 / 7.68; fast ≈ 22.32 / 12.74 / 7.69. If a class never appears in 10 boots, record that in the bench log; Task 2 then compares only the class it has.
- [ ] **Step 8: Version the baseline.** Copy the passing `m3-base-*` records, one JSON line each, from `results/bench.jsonl` into `docs/baseline-m3.jsonl`.
- [ ] **Step 9: Docs.**
  - `docs/bench-log.md`: a "M3 step 0" section with the spike table, the noise figures and the new baseline per class.
  - README:
    - a "Recommended config" section listing D-cache line 64 B (free, the largest lever), D-cache 64 KB (32 KB internal RAM) and `VJ_IRAM` (51 KB) or a 32 KB I-cache;
    - a note that with 64 B lines, PSRAM buffers used for DMA need 64 B alignment.
  - CLAUDE.md:
    - the trap "`idf.py -B dir flash` rebuilds from the current tree: flash `@flash_args`";
    - Status: step 0 done, the new baseline.
- [ ] **Step 10: Commit** `feat: M3 step 0 — decoder code in IRAM (VJ_IRAM), 64 KB/64 B D-cache in the bench, M3 baseline`.

### Task 2: One-command scorer

**Files:**
- Create: `tools/score.sh`
- Modify: `tools/compare.py` (exact fps from `total_us`; a `--gain PCT` acceptance mode)
- Modify: `tools/test_compare.py` (tests for both)

**Interfaces:**
- Consumes: `docs/baseline-m3.jsonl` (Task 1).
- Produces: `tools/score.sh LABEL` → exit 0 = KEEP, 1 = DROP (correct but not faster), 2 = FAIL (gate, build, flash or hashes). Every later task uses it.

- [ ] **Step 1: Failing checks.** `tools/test_compare.py` is a plain script: module-level asserts, no test runner, and an existing `rec(label, tuning, fps, ...)` helper. Append these module-level asserts after the existing ones, with their own helper names. Do not wrap them in `def test_*`: nothing would call them.
```python
def trec(label, tuning, rows):
    return {"label": label, "psram_tuning": tuning, "ok": True, "prof": [], "rows": rows}


def trow(stream, total_us, fps="0.00", frames=300):
    return {"stream": stream, "frames": f"{frames}/{frames}", "total_us": str(total_us), "fps": fps}


# exact fps from total_us: +0.310 % exact, but the 2-decimal tokens (30.09 vs 30.00) give 0.29999 %
assert compare.compare(trec("r", 5, [trow("s", 9_969_096, "30.09")]),
                       [trec("b", 5, [trow("s", 10_000_000, "30.00")])], None, gain=0.3)[0]
# --gain: one stream +11 %, the other -0.4 % -> rejected
assert not compare.compare(trec("r", 5, [trow("a", 9_000_000), trow("b", 10_040_000)]),
                           [trec("b", 5, [trow("a", 10_000_000), trow("b", 10_000_000)])], None, gain=0.3)[0]
# --gain: +0.1 % only -> rejected (no stream reaches +0.3 %)
assert not compare.compare(trec("r", 5, [trow("a", 9_990_000)]),
                           [trec("b", 5, [trow("a", 10_000_000)])], None, gain=0.3)[0]
```
- [ ] **Step 2: Run them.** `python3 -I tools/test_compare.py`. Expected: TypeError or AssertionError, because `compare()` has no `gain` parameter yet.
- [ ] **Step 3: Implement.**
  - In `compare.py`, `per_stream` computes fps as `int(row["frames"].split("/")[0]) * 1e6 / int(row["total_us"])` when `total_us` is present, falling back to `float(row["fps"])` for M1 records.
  - `compare(run, baseline, tol, gain=None)`: with `gain` set, ok iff every stream's Δ ≥ −gain and max Δ ≥ +gain; otherwise the existing ±tol rule.
  - CLI: `--gain PCT` (mutually exclusive with `--tol`). Lines print Δ to 3 decimals.
  - Keep the "run dropped streams" guard unchanged.
- [ ] **Step 4: Run the tests.** Expected: all `test_compare` tests pass, old ones included.
- [ ] **Step 5: `tools/score.sh`.**
```bash
#!/bin/bash
# usage: tools/score.sh LABEL [BASELINE]   exit 0 KEEP, 1 DROP (correct, not faster), 2 FAIL
# Host gate -> fresh IDF build -> flash @flash_args -> bench (search set) -> compare --gain 0.3.
set -uo pipefail
LABEL=$1; BASE=${2:-docs/baseline-m3.jsonl}
ROOT=$(cd "$(dirname "$0")/.." && pwd); cd "$ROOT"
IDF_PY=${IDF_PY:-$HOME/.espressif/python_env/idf5.4_py3.10_env/bin/python}
command -v idf.py >/dev/null || source "$HOME/esp/esp-idf-v5.4.2/export.sh" >/dev/null 2>&1
B="$ROOT/bench/build-score"
timeout 1200 make -C host check >/dev/null || { echo "FAIL host gate"; exit 2; }
rm -rf "$B"
idf.py -C bench -B "$B" -DSDKCONFIG="$B/sdkconfig" build 2>&1 | tr '\r' '\n' | grep -v '%)' | tail -2
[ -f "$B/videojet_bench.elf" ] || { echo "FAIL build"; exit 2; }
(cd "$B" && "$IDF_PY" -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 921600 write_flash @flash_args) \
  2>&1 | tr '\r' '\n' | grep -v '%)' | tail -2 || { echo "FAIL flash"; exit 2; }
"$IDF_PY" tools/bench.py --label "$LABEL" --set search --elf "$B/videojet_bench.elf" || { echo "FAIL bench"; exit 2; }
python3 tools/compare.py "$LABEL" --baseline "$BASE" --gain 0.3 && { echo KEEP; exit 0; }
echo DROP; exit 1
```
- [ ] **Step 6: Smoke-test.** Run `tools/score.sh m3-score-smoke` on the unchanged Task 1 tree. Expected: host gate ok, bench PASS, compare prints Δ ≈ 0 (|Δ| < 0.11 %) and exits 1 (DROP: no gain). That proves the gate, the flash identity and the noise floor end to end.
- [ ] **Step 7: Commit** `feat: tools/score.sh — one-command M3 scorer; compare.py exact fps and --gain mode`.

### Candidate protocol (Tasks 3–8)
Each candidate task runs this loop. The proposer is an **Opus** agent (CLAUDE.md: decoder internals).
1. The proposer branches `m3/<name>` from the current `m3-speed` tip, implements the one mechanism, adds the tests named in its task, and runs `make -C host check` (must pass).
2. The proposer writes a short design note into the task report: the invariant that keeps output bit-exact, and why.
3. The controller runs the task review, then `tools/score.sh m3-<name>`:
   - **KEEP:** merge into `m3-speed`, then re-baseline. Re-run the Task 1 Step 7 boot loop on the new tip as `m3-base-<name>-N`, and replace `docs/baseline-m3.jsonl` with those records in the merge commit.
   - **DROP:** keep the branch, and record the numbers and why in `docs/bench-log.md`'s M3 frontier table.
   - **FAIL:** fix rounds per the SDD loop. A hash mismatch is never accepted.
4. Dual-core candidates: `bench.py --set all` must PASS, plus 3 search boots with all hashes passing, before KEEP.
5. Every candidate row goes into the frontier table: candidate, fps per stream (exact), Δ %, `int_min`, IRAM bytes, verdict.

### Task 3: Skip elision + `changed_mb` (ladder step 2)
**Mechanism.** A pure-copy MB has every MV = (0,0) (the predicted MV, not the MVD), a single reference picture R and no residual (all totalCoeff 0). Typically that is a P_Skip, or a P_L0_16x16 with MV (0,0) and cbp = 0.
- Keep `u32 frameIdx[buf]` per DPB buffer: the decode-order index of the picture it holds, −1 if never written.
- Keep `u32 lastChanged[mb]` per MB: the index of the last picture whose final (deblocked) pixels at that MB differed from the previous picture's.
- **Elide** the prediction + write-out of a pure-copy MB when `frameIdx[X] != −1`, `frameIdx[R] != −1` and `lastChanged[mb] ≤ min(frameIdx[X], frameIdx[R])`, where X is the buffer being decoded. The pixels at that MB are then already identical in X and R. Deblocking still runs normally.
- **Update** after deblocking. An MB is unchanged in picture n only if all three hold, otherwise `lastChanged[mb] = n`:
  - it is pure-copy;
  - `lastChanged[mb] ≤ frameIdx[R]`;
  - no edge touching it had bS > 0. Record this as a per-MB "touched" bit set while filtering, for the MB and the neighbour across each edge.
- **Invalidate** by setting `lastChanged[*] = n` on an IDR, on any picture with concealment or errors, and on any SPS (re)activation. Allocation-time buffers start at −1.
- Export `vj_frame_t.changed_mb`: a `picSizeInMbs` byte map, 1 where `lastChanged == n`, valid until the next call. Document it in `videojet.h` and the README.

**Files:** `h264bsd_inter_prediction.c` (elision), `h264bsd_deblocking.c` (touched bits), `h264bsd_storage.{c,h}` / `h264bsd_dpb.c` (frameIdx, lastChanged, reset paths), `src/videojet.c` (changed_mb export), `host/test_api.c`.

**Tests:**
- The host gate (all 11 streams bit-exact, including the multi-ref JVT ones).
- A `test_api` case: decode `aux_card`, then assert that `changed_mb` is non-NULL and that some frame has changed-MB count < picSizeInMbs.
- A `test_api` case: a concealed picture marks every MB changed.

**Expected:** large on aux_card (67 % `inter`, mostly static skips) and on the black margins of aux_map; small on full_map.

### Task 4: Row-lagged deblock on a worker core (ladder step 3)
**Mechanism.** The worker filters MB row r once row r+1 is fully decoded, in raster order. This is bit-exact: intra prediction reads unfiltered neighbours from the picture (`h264bsdGetNeighbourPels`, `h264bsd_intra_prediction.c:545`); a row's vertical edges touch its bottom pixel line, which row r+1's intra prediction reads; inter prediction reads only finished reference pictures.
- The decode loop publishes rows completed (`h264bsd_slice_data.c`). At picture end the caller waits for the worker before DPB marking, so `vj_decode_au` returns with nothing in flight.
- Any error or concealment in the picture: wait for the worker, then filter the remaining rows sequentially after concealment, as today.
- Kconfig:
  - `VJ_DEBLOCK_WORKER` (bool, default n);
  - `VJ_WORKER_CORE` (0/1, default 0);
  - the worker priority is the creating task's priority − 1, minimum 1 (Jet lesson: the worker runs below the caller). There is no Kconfig knob for it;
  - worker stack ≤ 3 KB in internal RAM, counted in the budget.
- The worker is a FreeRTOS task on ESP and a pthread on the host. That way the host gate runs the threaded path under `-fsanitize=thread` (a `make -C host check-tsan` target).

**Files:** `h264bsd_deblocking.{c,h}` (row-range filter), `h264bsd_slice_data.c` (publish rows), `h264bsd_decoder.c` (picture-end wait), new `src/vj_worker.{c,h}` (task/pthread shim), `Kconfig`, `host/Makefile`.

**Tests:**
- The host gate with the worker on;
- `check-tsan` clean on all 11 streams;
- on the board: `--set all` plus 3 search boots.

**Expected:** deblock is 16–23 % of the frame, so ideally +19…+29 % fps, less any contention on the shared cache.

### Task 5: Direct-to-picture prediction for residual-free inter MBs
**Mechanism.** `h264bsdInterPrediction` predicts into the 384 B `data` buffer, then `h264bsdWriteMacroblock` copies it into the picture (`h264bsd_inter_prediction.c:470-480`). For MBs with no residual (P_Skip, or cbp = 0), predict straight into the picture rows instead. That is one PSRAM copy less per MB.
**Files:** `h264bsd_inter_prediction.c`, `h264bsd_reconstruct.c` (a destination-stride variant of `h264bsdPredictSamples`).
**Tests:** host gate; board.
**Expected:** a share of `inter` on the moving streams (full_map, aux_map).

### Task 6: CAVLC / bit-reader speedups
**Mechanism.** Exp-Golomb and coeff_token decoding through `__builtin_clz` (NSAU on Xtensa) on a 32-bit cached window, plus hot VLC tables `DRAM_ATTR`. One mechanism per sub-candidate: 6a is the bit reader, 6b the tables.
**Files:** `h264bsd_stream.c`, `h264bsd_vlc.c`, `h264bsd_cavlc.c`.
**Tests:** host gate; the board.
**Expected:** parse is 15–16 % of the map streams.

### Task 7: Profile split of `inter` + C motion-compensation kernels
**Step A, no fps claim.** Add `VJP_MC` (inside `h264bsdPredictSamples`) and `VJP_WRITE` (write-out) counters to `vj_prof.h`. Profile at the M3 tip and log the split in `docs/bench-log.md`.
**Step B, the candidate.** Speed up the hottest interpolation paths that Step A names (the 6-tap luma `h264bsdInterpolate*`, chroma) in plain C: 32-bit SWAR loads and row-wise reuse. Every kernel is checked against the existing function by an exhaustive host test over fractional positions, block sizes and edge (FillBlock) cases.

### Task 8: PIE kernels for the Task 7 hotspots (ladder step 5)
**Mechanism.** Hand-written S3 PIE asm for the kernels that Task 7 Step A ranks top, following the Jet PIE rules in CLAUDE.md (16 B-aligned destinations, 8 px blocks, scalar tails, exhaustive scalar-vs-SIMD tests). Espressif's encoder ships PIE quarter-pel MC (`McHorVer*_esp32s3`), so the approach is proven on this chip; none of its code is used.
**Tests:** an on-board kernel test app (C oracle against PIE, exhaustive), plus host gate and bench.

## Close-out
- [ ] Held-out run: `--set all` on the final `m3-speed` tip. Every hash must pass. Log the held-out fps.
- [ ] Final whole-branch review on the most capable model; one fix wave.
- [ ] Update the CLAUDE.md status, the m1-carryover M3 items closed or moved, and the bench-log frontier table.
