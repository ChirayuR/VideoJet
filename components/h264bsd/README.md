# h264bsd (vendored)

Android's Hantro H.264 baseline decoder, standalone fork. Apache-2.0 (LICENSE.md).
Upstream: https://github.com/oneam/h264bsd @ 42bcb5d753ad86d84903354bf3c68423c28adb7b (src/ only).

## Local changes
Every change made for VideoJet is listed here, newest last.
- VJ_PROFILE cycle counters around parse/recon/inter/deblock call sites (compiled out by default).
  New file `src/vj_prof.h`; in `h264bsd_slice_data.c`, `h264bsd_macroblock_layer.c` and `h264bsd_decoder.c` only an
  `#include "vj_prof.h"` line and the `{ VJP_BEGIN(..); <original call>; VJP_END(..); }` blocks.
