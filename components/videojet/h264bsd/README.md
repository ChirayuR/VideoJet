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
- Apache-2.0 §4(b) modification notice: every file changed above carries the header line
  `Modified for VideoJet: see h264bsd/README.md, Local changes.` Comment-only.
