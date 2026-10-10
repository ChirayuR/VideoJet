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
      (`h264bsdDeblockRecord`), in internal RAM (`vj_alloc_internal`). The pass reads only records and skips MBs with
      no edge.
    - Records hold only while the picture's MBs are decoded once each in raster order. Otherwise (FMO or ASO decoded out of raster order, a redundant
      re-decode, a slice error, concealment) the pass remakes every record from storage.
    - `GetLumaEdgeThresholds` / `GetChromaEdgeThresholds` became `GetEdgeThresholds`, which works from qps.
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
    Bit-exact against the C kernels: the on-board app `test/mc_pie` compares them over test_mc.c's case space plus a
    6x4-MB picture (15.7M cases), and against the standard's equations on a sample (bench-log, T8). Any change to
    `vj_mc_pie.S`, or to the C kernels it mirrors, must re-run `test/mc_pie` on the board: `MC PASS`, 0 mismatches.
  - CAVLC symbol tables (`h264bsd_cavlc.c`). IDF builds with `-fno-jump-tables`, so the old `switch`es were compare
    trees. Each decoder returns the old information word for every input: `host/test_cavlc.c` holds the original
    routines and compares the two over every input.
    - run_before: one table indexed by `MIN(zerosLeft, 7)` and the next 3 bits. A 0 there is a code of 4 to 11 bits
      (zerosLeft > 6): `run_before` = leading zeros + 4.
    - total_zeros: a (table, shift) entry per totalCoeff replaces the `switch`; chroma DC has a table.
    - coeff_token, 0 <= nC < 4: codes over 6 bits come from one table indexed by the leading zeros (`__builtin_clz`)
      and the next 3 bits, in place of 2 or 3 more range tests and tables. The 1- and 2-bit codes keep their tests.
- Apache-2.0 §4(b) modification notice: every file changed above carries the header line
  `Modified for VideoJet: see h264bsd/README.md, Local changes.` Comment-only.
