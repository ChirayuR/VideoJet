# VideoJet

Fast H.264 constrained-baseline decoding for the ESP32-S3 (ESP-IDF 5.4 or newer): one Annex-B access unit (AU) in, one
decoded I420 picture out. The decoder inside is a private, vendored copy of h264bsd; you see `include/videojet.h` only.

## 1. Add it
- Before `project()` in your top-level `CMakeLists.txt`: `set(EXTRA_COMPONENT_DIRS /path/to/VideoJet/components)`, then
  `PRIV_REQUIRES videojet` in your component.
- Or a git dependency in your component's `idf_component.yml`: `videojet: {git: <repo url>, path: components/videojet}`.

## 2. Use it
```c
#include "videojet.h"

vj_decoder_t *dec = vj_decoder_create();
if (!dec)                                                /* out of memory */
    return;
bool need_idr = true;                                    /* frames are unreliable until an IDR arrives */
for (size_t off = 0, n; off < len; off += n) {           /* buf: the byte stream, in writable RAM */
    n = vj_au_length(buf + off, len - off);              /* one access unit; 0: no start code left */
    if (n == 0)
        break;
    vj_frame_t f;
    vj_status_t st = vj_decode_au(dec, buf + off, n, &f);
    if (st < 0) {                                        /* VJ_ERR_*: see the contract */
        need_idr = true;
        request_keyframe();
        continue;
    }
    if (st == VJ_FRAME && f.idr)
        need_idr = false;
    if (st == VJ_FRAME && !need_idr) /* the cropped picture; crop offsets are even, so the chroma offsets are exact */
        show(f.y + f.crop_y * f.y_stride + f.crop_x, f.u + (f.crop_y / 2) * f.uv_stride + f.crop_x / 2,
             f.v + (f.crop_y / 2) * f.uv_stride + f.crop_x / 2, f.y_stride, f.uv_stride, f.crop_w, f.crop_h);
}
vj_decoder_destroy(dec);
```

## 3. Requirements
- PSRAM in the heap: `CONFIG_SPIRAM=y` with `CONFIG_SPIRAM_USE_MALLOC` (the default) or `CONFIG_SPIRAM_USE_CAPS_ALLOC`.
  `CONFIG_SPIRAM_USE_MEMMAP` leaves the library no PSRAM to allocate from, so the frames fail with `VJ_ERR_NOMEM`.
- Memory at 800x480, measured on the board (`docs/bench-log.md`, M2 section): 1,507,340 B of PSRAM and 13,544 B of internal
  RAM, both heap, the same on every run and boot. The PSRAM holds the frames (about 1.5 x width x height bytes each,
  refs + 1 of them, so 2 for a one-reference stream) and the per-macroblock state (1500 MBs x 216 B). Each extra reference
  frame adds 589,828 B. IDF's heap rounds every block up to its size class, so the PSRAM figure is 2.1 % above the sum of
  the structure sizes.
- Stack: decoding runs on the stack of the task that calls `vj_decode_au()`. The stack it needs has not been measured; only a
  16 KiB task stack (the bench's) has been tested.
- The library places its own buffers, whatever your `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`: above 16 KiB PSRAM first, below it
  internal RAM first, each falling back to the other. It asks for the same memory as IDF's `malloc`, so it never takes the
  internal DMA reserve (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`).
- Recommended: 240 MHz CPU, octal PSRAM at 80 MHz, `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`).
  The speed reference is `docs/bench-log.md`.

## 4. Contract
- One AU per `vj_decode_au()` call; `vj_au_length()` splits a byte stream. Pictures come out in decode order.
- One decoder per task: calls on one decoder must not overlap.
- `f.y`, `f.u` and `f.v` are valid until the next `vj_decode_au()` or `vj_decoder_destroy()`.
- The call rewrites the AU in place (it removes emulation-prevention bytes): pass writable RAM, never flash-mapped data, and
  decode each AU once.
- `f.changed_mb` is NULL for now: treat every macroblock as changed.
- After any `VJ_ERR_*`, request a keyframe and treat frames as unreliable until one with `f.idr` arrives (a lost reference
  can leave `error_mbs` at 0). `error_mbs` counts concealed macroblocks in a returned frame and is rarely non-zero.

## 5. Kconfig (menu "VideoJet", both off by default)
- `CONFIG_VJ_ERROR_PRINT`: print the decoder's bitstream errors to stderr (about 60 messages, from the decoding task).
- `CONFIG_VJ_PROFILE`: per-stage cycle counters, read with `vj_profile_take()`. Their cost is not separable from code-layout
  noise (`docs/bench-log.md`) and they move code layout: profiling builds only, never timed scores (the bench enables it
  through `bench/sdkconfig.profile`).

## 6. Licensing
The h264bsd core is Apache-2.0 (`h264bsd/LICENSE.md`). The licence for VideoJet's own code is to be chosen by the owner
before publishing.
