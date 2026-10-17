# h264bsd (vendored)

Android's Hantro H.264 baseline decoder, standalone fork. Apache-2.0 (LICENSE.md).
Upstream: https://github.com/oneam/h264bsd @ 42bcb5d753ad86d84903354bf3c68423c28adb7b (src/ only).

## Local changes
Every change made for VideoJet is listed here, newest last.
- VJ_PROFILE cycle counters around parse/recon/inter/deblock call sites (compiled out by default).
  New file `vj_prof.h`; in `h264bsd_slice_data.c`, `h264bsd_macroblock_layer.c` and `h264bsd_decoder.c` only an
  `#include "vj_prof.h"` line and the `{ VJP_BEGIN(..); <original call>; VJP_END(..); }` blocks.
- Moved into `components/videojet/h264bsd/` (was `components/h264bsd/src/`). The old component is gone: `videojet` builds
  every `h264bsd/*.c` as private code.
- Allocations go through `vj_alloc()` (`components/videojet/src/vj_alloc.h`), so the library, not the consumer's malloc
  settings, decides internal RAM versus PSRAM: `h264bsd_util.h` gains `#include "vj_alloc.h"` and its `ALLOCATE` macro calls
  `vj_alloc`; `mbLayer` in `h264bsdInit` and the storage in `h264bsdAlloc` do too. `free()` releases either heap. The
  `conversionBuffer` mallocs serve the unused RGBA converters and are left.
- `vj_prof.h` keys on `CONFIG_VJ_PROFILE` (a Kconfig switch, off by default), not on `-DVJ_PROFILE`.
- Parameter-set status in `h264bsdDecode` (`h264bsd_decoder.c`):
  - `h264bsdStoreSeqParamSet` / `h264bsdStorePicParamSet` returned `MEMORY_ALLOCATION_ERROR` when the SPS/PPS slot could not
    be allocated, and the caller ignored it: the parsed set's arrays leaked (the SPS's VUI is 952 B, plus
    `offsetForRefFrame`; the PPS's slice-group arrays) and the NAL unit reported success. It now frees those arrays and
    returns `H264BSD_MEMALLOC_ERROR`.
  - `MEMORY_ALLOCATION_ERROR` from `h264bsdDecodeSeqParamSet` / `h264bsdDecodePicParamSet` (a VUI or array allocation) now
    also returns `H264BSD_MEMALLOC_ERROR` instead of `H264BSD_ERROR`, so every allocation failure reaches the caller as
    "out of memory", not "bad stream".
- M3 speed changes. Each one is bit-exact on the host gate and the board bench (`docs/bench-log.md`, M3 frontier):
  - Direct skip copy (`h264bsd_inter_prediction.c`): a P_Skip MB with an even integer MV whose block lies inside the
    reference is copied reference → picture, with no staging in `data[384]`.
  - No per-picture `mbStorage_t` clear (`h264bsd_storage.c/h`, `h264bsd_slice_data.c`, `h264bsd_conceal.c`,
    `h264bsd_deblocking.c/h`, `h264bsd_decoder.c`).
    - `storage_t.picSliceBase` is the per-picture "slice id 0". Slice ids only grow, and `decoded` is reset lazily in
      SetMbParams.
    - Concealed MBs that no slice touched take `sliceId = picSliceBase`.
    - The deblock slice-boundary tests compare `MAX(id, base)`.
    - The original clear runs only when ids reach 0x80000000.
  - Skip elision (`h264bsd_dpb.c/h`, `h264bsd_inter_prediction.c`, `h264bsd_deblocking.c`, `h264bsd_decoder.c`).
    - `dpbPicture_t.vjFrameIdx` labels each buffer with the picture it holds, and a per-MB counter records the last
      picture whose pixels changed there.
    - A pure-copy MB whose pixels are already in the target buffer is not rebuilt. Deblocking marks the MBs it
      changes, and the map is exported as `vj_frame_t.changed_mb`.
  - Lazy `macroblockLayer_t` clear (`h264bsd_macroblock_layer.c`): only the fields a reader can see unwritten are
    zeroed. Uncoded blocks depend on `h264bsdProcessBlock`'s `coeffMap == 0` path.
  - Deblock records (`h264bsd_deblocking.c/h`, `h264bsd_slice_data.c`, `h264bsd_storage.c/h`, `h264bsd_decoder.c`,
    `h264bsd_conceal.c`).
    - Each MB's bS (2 bits per edge segment) and threshold inputs go into a 12 B record when it is decoded
      (`h264bsdDeblockRecord`), in internal RAM (`vj_alloc_internal`; PSRAM-first, `vj_alloc_psram`, with
      `VJ_PARSE_PIPELINE`, whose ring needs the room). The pass reads only records and skips MBs with no edge.
    - Records hold only while the picture's MBs are decoded once each in raster order. Otherwise (FMO or ASO decoded out of raster order, a redundant
      re-decode, a slice error, concealment) the pass remakes every record from storage.
    - `GetLumaEdgeThresholds` / `GetChromaEdgeThresholds` became `GetEdgeThresholds`, which works from qps.
  - Cheap skip record (`h264bsd_deblocking.c/h`, `h264bsd_slice_data.c`): `h264bsdDeblockRecord(..., justDecoded)` takes a
    shortcut for a P_Skip MB right after it decoded OK (totalCoeff is all 0, mvs and refs uniform, so the inner edges are
    bS 0): only the two MB edges are evaluated, with the same `EdgeBoundaryStrength` tests. The pass's remake-from-storage
    path passes 0 and runs the generic code, because stale storage after a failed decode need not be uniform.
  - Bit reader (`h264bsd_stream.c/h`, `h264bsd_vlc.c`, `h264bsd_cavlc.c`, `h264bsd_util.c`, `h264bsd_byte_stream.c`,
    `h264bsd_decoder.c`, `h264bsd_macroblock_layer.c`).
    - `strmData_t` holds the read position in `strmBuffReadBits` alone, plus a window `cache[2]`: the two stream words
      from `strmBuffReadBits / 32`, first byte in the MSB, 0 past the end. `h264bsdShowBits32` and `h264bsdFlushBits` are
      `static inline` on it. `h264bsdStrmFill` reloads it when a flush enters another word, after NAL extraction (which has
      already stripped the 0x03 bytes in place) and on the re-decode path.
    - `pStrmCurrPos` and `bitPosInWord` are gone: `h264bsdIsByteAligned` and `h264bsdRbspTrailingBits` use
      `strmBuffReadBits & 7`. So is the macroblock layer's resync of `strmBuffReadBits` after the residual: an identity
      after success, and after an error the slice's stream is never read again.
    - Exp-Golomb codes (up to 31 leading zeros) and CAVLC's level_prefix count the zeros with `__builtin_clz`
      (NSAU on Xtensa).
  - Profile split of `inter` (`h264bsd_inter_prediction.c`, `h264bsd_reconstruct.c`, `vj_prof.h`; `VJ_PROFILE` builds only,
    no change to the default build): `{ VJP_BEGIN(..); <original code>; VJP_END(..); }` blocks around MV prediction, the
    direct skip copy and the write-out, and in `h264bsdPredictSamples` around the whole function, the luma part
    (counted per quarter-sample position) and `PredictChroma`.
  - Luma motion compensation (`h264bsd_reconstruct.c/h`), bit-exact: `host/test_mc.c` compares `h264bsdPredictSamples`
    with the standard's interpolation equations over every quarter/eighth-sample position, partition, offset and
    picture-edge case (the original kernels pass it too).
    - `h264bsdPredictSamples` takes one reference window per partition, read in place or padded by `h264bsdFillBlock`
      (`LumaEdge`), and dispatches to C kernels that keep a 6-sample window in registers and clamp arithmetically. The
      centre positions sum one way into an i16 table (`LumaCentre`), then filter the other way.
    - The eight luma `h264bsdInterpolate*` functions and `lumaFracPos` are gone. Chroma (`h264bsdInterpolateChroma*`,
      `PredictChroma`) is unchanged.
    - Upstream's left shifts of negative values (UBSan shift-base) in this file went with the old kernels.
  - PIE motion compensation (`h264bsd_reconstruct.c`; kernels in VideoJet's own `src/vj_mc_pie.S`, `CONFIG_VJ_PIE`,
    ESP32-S3 only, default y). For 8 and 16 wide partitions whose reference window lies inside the picture,
    `h264bsdPredictSamples` calls the PIE kernels (`LumaInside`, `ChromaPie`); edge windows, 4 wide partitions and
    the host build keep the C kernels. The kernels read whole 16-byte chunks: up to 47 bytes past a luma row start and
    31 past a chroma row start, inside the DPB buffers (luma is followed by chroma; 32 bytes of slack after Cr).
    The destination must be 16-byte aligned (PIE stores round the address down): the dispatch checks and falls back to
    C, and `ConcealMb`'s `data[384]` is `aligned(16)` (it was aligned only by luck, not under IDF stack protector modes).
    Bit-exact against the C kernels: the on-board app `test/mc_pie` compares them over test_mc.c's case space plus a
    6x4-MB picture (15.7M cases), and against the standard's equations on a sample (bench-log, T8). Any change to
    `vj_mc_pie.S`, or to the C kernels it mirrors, must re-run `test/mc_pie` on the board: `MC PASS`, 0 mismatches.
  - PIE write-out (`h264bsd_image.c`; kernels in `src/vj_wr_pie.S`, `CONFIG_VJ_PIE`). `h264bsdWriteOutputBlocks` calls
    `vj_pie_write_out`: per 4x4 block an empty residual copies the prediction rows, else the residual (low 16 bits of
    each i32) is added to the zero-extended prediction in s16 lanes with saturation, clipped 0..255 and stored; the C
    reads a 1280-entry clip table, so the two agree for every pred + residual in -512..767 (all valid streams; out of
    that range the C reads outside its table). `h264bsdWriteMacroblock` stays C (a PIE 16 B row copy measured equal, 336 vs 342
    cycles; an all-empty pre-scan cost more than it saved). Exact loads/stores, no over-read. The dispatch falls back to C unless the residual array is 16-aligned
    (ee.vld.128 rounds down) and the pointers 4-aligned.
    Oracle: the on-board app `test/wr_pie` (PIE vs the same file compiled with PIE off, guard bytes, all (pred, residual)
    pairs, every empty-block pattern, pointer offsets) must print `WR PASS`; the C path has `host/test_write.c`.
  - CAVLC symbol tables (`h264bsd_cavlc.c`). IDF builds with `-fno-jump-tables`, so the old `switch`es were compare
    trees. Each decoder returns the old information word for every input: `host/test_cavlc.c` holds the original
    routines and compares the two over every input.
    - run_before: one table indexed by `MIN(zerosLeft, 7)` and the next 3 bits. A 0 there is a code of 4 to 11 bits
      (zerosLeft > 6): `run_before` = leading zeros + 4.
    - total_zeros: a (table, shift) entry per totalCoeff replaces the `switch`; chroma DC has a table.
    - coeff_token, 0 <= nC < 4: codes over 6 bits come from one table indexed by the leading zeros (`__builtin_clz`)
      and the next 3 bits, in place of 2 or 3 more range tests and tables. The 1- and 2-bit codes keep their tests.
  - Inverse transform / dequant (`h264bsd_transform.c`, `h264bsdProcessBlock` only; `src/vj_idct_pie.S/.h`).
    - C: the scan, scaling and both passes run in registers on u32 (no intermediate store/reload), level scales come
      pre-shifted from a 52x3 u16 table, and the range check ORs (value + 512) over the block. On HANTRO_NOK the block
      contents are unspecified (the original left it half transformed); the caller abandons the macroblock. The
      `coeffMap` paths (full, rows 1-3 zero, DC only) are unchanged, so uncoded blocks still rely on the `coeffMap == 0`
      DC-only path.
    - PIE (`CONFIG_VJ_PIE`), full path only: the 16 products go (scalar) to a transposed, aligned scratch block;
      `vj_pie_idct4` does the horizontal pass lane-wise, transposes with four `ee.vzip.32`, does the vertical pass and
      stores the four output rows. Its arithmetic saturates where the C code wraps, so it first checks every product is
      within +-2^26 and returns "guard failed" (nothing stored) otherwise, and C runs. `data` must be 16-aligned
      (`macroblockLayer_t.level` is `aligned(16)`, and `mbLayer` comes from `vj_alloc_aligned16`), else C.
    - `host/test_idct.c` holds the original and compares return value and outputs over every coeffMap x qp x skip,
      extreme and guard-edge coefficients, aligned and misaligned blocks. Any change to `vj_idct_pie.S` or to that
      full path must re-run `test/idct_pie` on the board: `IDCT PASS`, 0 mismatches.
- Apache-2.0 §4(b) modification notice: every file changed above carries the header line
  `Modified for VideoJet: see h264bsd/README.md, Local changes.` Comment-only.
  - Row-lagged deblock worker (`VJ_DEBLOCK_WORKER`; `h264bsd_deblocking.c/h`, `h264bsd_decoder.c`, `h264bsd_slice_data.c`,
    `h264bsd_conceal.c`, `h264bsd_storage.h`, new `src/vj_worker.c/h`).
    - `h264bsdFilterPicture`'s body is `FilterMbRows` (row range, optional undo log); the picture's rows are filtered by a
      worker thread while the decoder decodes later rows (row r once row r + 1 is decoded), through the decode-time
      records. At picture end the decoder waits for the worker; no work is in flight when `h264bsdDecode` returns
      anything but `H264BSD_RDY` (and `vj_decode_au` joins on every path).
    - Error paths: a slice error calls `h264bsdDbwAbortError` (waits for the worker, puts back the footprints it logged
      for rows posted under that slice, drops the marks, resumes the final pass at the first row of the slice); concealment
      and a picture that left raster order call `h264bsdDbwAbortKeep`. No worker for FMO/ASO or redundant-slice pictures,
      and I slices are filtered at the end of the picture. The correctness argument is at the top of the worker section
      of `h264bsd_deblocking.c`; the host gate runs it threaded (pthread) and `make -C host check-tsan` under TSan.
    - T3 marks of the rows the worker filters go to its own array and join `changed` when the picture ends.
  - Parse records (`h264bsd_parse.h` new, `h264bsd_slice_data.c`, `h264bsd_macroblock_layer.c/h`, `h264bsd_storage.c/h`,
    `h264bsd_decoder.c`). The macroblock loop's bitstream side is `vjParseNext`: one call fills one ring slot
    (`vjSlot_t`, a `macroblockLayer_t` plus kind) with a parsed MB, a whole skip run (SKIPRUN, count) or an error (ERR, the
    status the loop used to return), and `last` = no stream data follows. `h264bsdDecodeSliceData` fetches a record
    where it used to parse and runs the same checks, `SetMbParams`, recon and record code in the same order. The parser
    writes nothing but its slot and state. nC (`DetermineNc`) takes the neighbours' totalCoeff as `tcA` / `tcB` (`vjTc_t`):
    without the pipeline the storage's own i16 array, as the original read it (one slot, no shadow, a skip run costs O(1));
    with it a parser-private u8 shadow (`vjNbr`, width + 1 entries, cleared per slice, a skip run fills the last width + 1
    entries, only the 13 entries nC can read are copied: `test_nbr`). With slice groups the storage is read in both builds.
    `mbLayer` became the ring (`vjRing`). The H264DEC_OMXDL branches are dead code and no longer compile consistently
    (`DecodeResidual` / `DetermineNc` take the neighbours' totalCoeff instead of `mbStorage_t *` in the non-OMXDL build only); left alone.
  - Parse pipeline (`VJ_PARSE_PIPELINE`; `h264bsd_slice_data.c`, `h264bsd_parse.h`, `h264bsd_deblocking.c/h`,
    `h264bsd_decoder.c`, `src/vj_worker.c/h`). The deblock worker's task also runs `vjParseStep`: one record per call into the
    ring (depth 6), published with a release store of `head`; the decoding task polls `head`, reconstructs the slot in place
    and frees it with `tail`. The parse context changes hands through `pstate` (IDLE -> ARMED by a release store, back by
    compare-and-swap, spinning while a record is being parsed), and `h264bsdDecodeSliceData` hands it back on every exit,
    so no worker step is in flight when the NAL buffer is released. Records are a pure function of the stream, so the
    schedule cannot change output, status or storage. One slice group and primary slices only; the worker's core must
    differ from the caller's; no record for 50 ms: the decoding task takes the context back, parses inline, and parses the next
    30 slices inline before it asks the worker again. A worker that finds the ring full for ~200 us announces a sleep (a flag, a
    full fence, a re-check of `tail`) and blocks; the decoding task, after freeing a slot (release store of `tail`) and a full
    fence, wakes it only if the flag is set. The worker's run is now `DbwRun` in segments of 10 MBs (`FilterMbRows` takes an MB
    range) so the task can parse between them. Test hooks, each used by a committed test: `VJ_POISON`, `VJ_NBR_STORAGE`,
    `VJ_PIPE_JITTER_US`, `VJ_PIPE_NOSTEP`, `VJ_PIPE_STALL_EVERY`, `VJ_RING_DEPTH` (see `host/Makefile`).
  - NAL extraction (`h264bsd_byte_stream.c`, `h264bsdExtractNalUnit`). The start-code search keeps its byte loop (the
    start code is a few bytes in). The NAL-end scan and the emulation-byte removal work on zero runs: only a run of two or
    more zeros can matter, and only the byte that ends it (01 = the next start code; 03 after exactly two zeros = an
    emulation byte; anything else after three or more = invalid stream), so the bytes that are not zero are skipped four
    at a time (`SkipNonZero`: a scalar head to a word boundary, aligned 32-bit loads, `(w - 0x01010101) & ~w &
    0x80808080` as the has-a-zero-byte test; nothing outside the buffer is read). The removal pass moves the bytes
    between emulation bytes with one `memmove` each (none before the first), and an error still leaves the bytes before
    the offending one moved down, as the byte loop did.
    - `memmove` is the S3 ROM's byte loop (about 8-10 cycles per byte), so an NAL with an early emulation byte pays that
      for every byte after it (still less than the two byte passes it replaces); a word-copy helper would be the next
      step if emulation-heavy streams matter.
    - One `SkipNonZero` call per zero run: with a zero byte every 4th byte or denser it is no faster than the byte loop
      (x86 proxy: 0.94x at every 4th, 0.3x at every 2nd; every 16th 4x faster, no zeros 5.7x). The corpus has one zero run
      per ~80 bytes (real AA captures are not measured yet).
    - The object is mapped to flash (`components/videojet/linker.lf`, with the other once-per-NAL-or-less code): one call per
      NAL, and its loop runs from the I-cache. So the 645 B of the new code are not added to internal RAM and the 337 B of
      the old function are freed (the internal RAM budget, ruling R12).
    - Identical to the byte-at-a-time original for every input (status, `*readBytes`, every field of `strmData_t`, the
      buffer afterwards): `host/test_nal.c` holds the original and compares them on every buffer of length 1..8 over
      {00 01 02 03 04 FF} (and 9..13 over smaller alphabets), 10 M random ones and 4000 long ones, each at 4 alignments with
      a varying filler below the buffer. It runs once in `make check` (it does not depend on the worker or the pipeline). A
      read past the end of the buffer is seen only by ASan: an edit of this file must pass `make check-asan` too.
  - Hold (`vj_decoder_set_hold`; `h264bsd_dpb.c/h`, `h264bsd_decoder.c`). The frame returned last, `dpbStorage_t.vjHeld`
    (set by `src/videojet.c`), is neither decoded into nor freed until another frame is returned.
    - `h264bsdInitDpb` makes one more frame buffer, the spare `buffer[dpbSize + 1]` (the array keeps its 17 entries unless
      dpbSize is 16), and every frame buffer gets `picSizeInMbs` more bytes after its slack: the frame's copy of
      changed_mb, made when it is returned (`h264bsdHoldFrame`).
    - `h264bsdAllocateDpbImage`: when `buffer[dpbSize]` holds vjHeld (a non-reference frame, or a reference that MMCO or a
      frame_num gap unmarked), it trades places with the spare (`h264bsdHoldSwap`, whole entries: both are UNUSED, and
      data, pAllocatedData and vjFrameIdx move together, so skip elision still knows what each buffer holds). The spare is
      outside ShellSort, the marking and the reference lists.
    - `h264bsdFreeDpb` on a reset frees every buffer but vjHeld's, which becomes `vjOrphan`: `h264bsdHoldRelease` frees it at
      the start of the first call after another frame was returned, `h264bsdShutdown` at the latest.
    - `h264bsdRecordChanges`' renumbering restart includes the spare.
    - Hold off: vjHeld stays NULL and there is no spare, so nothing else changes.
    - `h264bsdInitDpb`, `h264bsdResetDpb`, `h264bsdFreeDpb`, `h264bsdShutdown` and the three hold functions run from flash
      (`linker.lf`, symbol entries): once per SPS activation, reset or destroy, and with hold once per frame returned or
      non-reference frame. IRAM went down by 388 B (bench configuration).
    - `host/test_hold.c` decodes every stream (clean, one NAL per call, damaged, across picture-size changes) with hold off
      and on in lockstep and checks the held frame after every call; it runs in `make check`, `check-asan`, `check-tsan`.
  - Skip-run path (`h264bsd_slice_data.c`, `h264bsd_macroblock_layer.c/h`, `h264bsd_inter_prediction.c/h`,
    `h264bsd_deblocking.c/h`, `h264bsd_storage.c`, `h264bsd_conceal.c`, `src/vj_alloc.c/h`). A skipped MB whose motion
    vector the P_Skip rule makes 0 without a median (its left or upper neighbour is unavailable, or is a skip of the same
    slice with MV 0) is decoded by `DecodeSkipRun`: the lazy `decoded` clear and `SetMbParams`, the P_Skip fields, the
    skip elision or direct copy and the deblock record, with the generic loop's results. The MBs of a run before its last
    go through it in one loop, without the generic loop's head and tail (they fetch and consume no record and the next
    address is + 1); the first and last MB of a run go through it one at a time inside the generic iteration (not
    retried for an MB the run loop just stopped at). It stops at an MB the generic loop would fail on (decoded already)
    and leaves it to the generic path; one slice group, primary P slices, reference 0 present. It mirrors four generic
    sites, each marked "mirrored in DecodeSkipRun": `SetMbParams`, the P_Skip path of `h264bsdDecodeMacroblock` with the
    stores of `MvPrediction16x16`, the elision/copy decision of `h264bsdInterPrediction`, and the loop's record and row
    posting.
    - `mbStorage_t` is reordered: what such an MB writes or its neighbours look up comes first (`mbType`, `sliceId`,
      `decoded`, `refAddr`, then `qpY`, `disableDeblockingFilterIdc`, `filterOffsetA/B`, `chromaQpIndexOffset` and
      `refPic` narrowed to 8 bits, which hold every value they get: qp 0..51, idc 0..2, offsets -12..12, refIdx 0..15, all
      checked when parsed; then `mbA`..`mbD`). The struct is `aligned(64)`, 192 B on the S3 (216 before), `mv` stays
      4-aligned, and `mb[]` comes from `vj_alloc_aligned64`, so those fields are one cache line per MB.
    - `vjZero`: the MB is a zero-MV P_Skip and its `totalCoeff`, `refPic` and `mv` are all 0. `h264bsdDecodeMacroblock`
      (whose call tree and the skip-run path are the only writers of those fields) and `ConcealMb` (which makes the MB
      intra) clear it, the P_Skip branch of `h264bsdInterPrediction` sets it again for MV 0 and refIdx 0, and the skip-run
      path writes the three only when it is clear. An MB that was such a skip in the picture before is then decoded
      touching its first line only.
    - `h264bsdDeblockRecord`'s P_Skip branch is `h264bsdDeblockRecordSkip` (the generic record calls it for a P_Skip just
      decoded, the skip-run path directly): it tests the MB edges with `SkipEdgeBs` (the edge test for a skip's uniform
      mv and reference, as before) and reads neither a zero-MV skip's own mv nor a zero-MV skip neighbour's totalCoeff
      and mvs (all 0): on such an edge only the references and the MB's mv can differ. `ASSERT`s
      (`h264bsdZeroSkipFields`) check the claim.
    - `h264bsdCopyMb`, the direct copy, is shared by both paths: word loads and stores when the source is word-aligned
      (x a multiple of 8, `ASSERT`ed), else the old `memcpy` rows.
    - `host/fz.c` hashes the storage fields by value, so storage hashes stay comparable with trees before the narrowing.
  - CAVLC residual per macroblock (`h264bsd_cavlc.c/h`, `h264bsd_macroblock_layer.c`, `h264bsd_neighbour.h`).
    `h264bsdDecodeResidualCavlc` replaces `DecodeResidual`, `DetermineNc` and the per-block
    `h264bsdDecodeResidualBlockCavlc` (kept in `host/test_cavlc.c` as the oracle): one call per macroblock, the
    `strmData_t` window (`strmBuffReadBits`, `cache[0..1]`) in registers, a funnel-shift peek (`ssl` + `src` on the
    S3) and a skip per symbol, nC from two 24-entry tables, a level as the top bits of the window less its prefix
    zeros, the levels after a block's first from `vjLevelTab` when their code fits in 6 bits, and run_before placing
    the levels from the highest position down. Same symbols, tables, arithmetic, checks and stored levels; the
    end-of-buffer check is per block (positions only grow, so the old check at each refill gave the same
    `HANTRO_NOK`), and on an error the levels hold an unspecified part of the block (a failed macroblock is never
    reconstructed). `neighbour_t` holds its `neighbourMb_e` in a `u8`: the neighbour tables take 2 B an entry, not 8.
