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
  settings, decides internal RAM versus PSRAM: the `ALLOCATE` macro in `h264bsd_util.h`, `mbLayer` in `h264bsdInit`, and the
  storage in `h264bsdAlloc`. `free()` releases either heap. The `conversionBuffer` mallocs serve the unused RGBA converters
  and are left.
- `vj_prof.h` keys on `CONFIG_VJ_PROFILE` (a Kconfig switch, off by default), not on `-DVJ_PROFILE`.
- Parameter-set status in `h264bsdDecode` (`h264bsd_decoder.c`):
  - `h264bsdStoreSeqParamSet` / `h264bsdStorePicParamSet` returned `MEMORY_ALLOCATION_ERROR` when the SPS/PPS slot could not
    be allocated, and the caller ignored it: the parsed set's arrays leaked (the SPS's VUI is 952 B, plus
    `offsetForRefFrame`; the PPS's slice-group arrays) and the NAL unit reported success. It now frees those arrays and
    returns `H264BSD_MEMALLOC_ERROR`.
  - `MEMORY_ALLOCATION_ERROR` from `h264bsdDecodeSeqParamSet` / `h264bsdDecodePicParamSet` (a VUI or array allocation) now
    also returns `H264BSD_MEMALLOC_ERROR` instead of `H264BSD_ERROR`, so every allocation failure reaches the caller as
    "out of memory", not "bad stream".
