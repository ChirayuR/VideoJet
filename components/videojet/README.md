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
    if (st == VJ_FRAME && f.error_mbs) {                 /* concealed macroblocks, even in an IDR: a keyframe cue too */
        need_idr = true;
        request_keyframe();
    }
    if (st == VJ_FRAME && !need_idr) /* the cropped picture; crop offsets are even, so the chroma offsets are exact */
        show(f.y + f.crop_y * f.y_stride + f.crop_x, f.u + (f.crop_y / 2) * f.uv_stride + f.crop_x / 2,
             f.v + (f.crop_y / 2) * f.uv_stride + f.crop_x / 2, f.y_stride, f.uv_stride, f.crop_w, f.crop_h);
}
vj_decoder_destroy(dec);
```

## 3. Requirements
- PSRAM in the heap: `CONFIG_SPIRAM=y` with `CONFIG_SPIRAM_USE_MALLOC` (the default) or `CONFIG_SPIRAM_USE_CAPS_ALLOC`.
  `CONFIG_SPIRAM_USE_MEMMAP` leaves the library no PSRAM to allocate from, so the frames fail with `VJ_ERR_NOMEM`.
- Memory at 800x480, measured on the board (`docs/bench-log.md`, M2 and M3 step 0 sections): 1,507,340 B of PSRAM and
  13,544 B of internal RAM, both heap, the same on every run and boot. The PSRAM holds the frames (about 1.5 x width x height
  bytes each, refs + 1 of them, so 2 for a one-reference stream) and the per-macroblock state (1500 MBs x 216 B). Each extra
  reference frame adds 589,828 B. IDF's heap rounds every block up to its size class, so the PSRAM figure is 2.1 % above the
  sum of the structure sizes. On top of the heap, `CONFIG_VJ_IRAM` (the default) puts the decoder's code, 50,831 B, in
  internal RAM: 13,544 + 50,831 = 64,375 B in all, excluding the stack.
- Stack: decoding runs on the stack of the task that calls `vj_decode_au()`. The stack it needs has not been measured; only a
  16 KiB task stack (the bench's) has been tested.
- The library places its own buffers, whatever your `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`: above 16 KiB PSRAM first, below it
  internal RAM first, each falling back to the other. It asks for the same memory as IDF's `malloc`, so it never takes the
  internal DMA reserve (`CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL`).

## 4. Recommended config
The bench's `sdkconfig.defaults` is the reference. Together the three cache and placement settings below took
aux_card / aux_map / full_map (800x480) from 17.11 / 9.67 / 5.58 fps (M2: code in flash, 16 KB I-cache, 32 KB D-cache with
32 B lines) to 22.21 / 12.72 / 7.68 (+30 / +32 / +38 %). Numbers and records: `docs/bench-log.md`, M3 step 0.
- 240 MHz CPU, octal PSRAM at 80 MHz, `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`).
- **D-cache line 64 B** (`CONFIG_ESP32S3_DATA_CACHE_LINE_64B=y`): the largest lever, and it costs no RAM. At 32 KB with the
  code in IRAM it gained +19 / +11 / +7 %.
- **D-cache 64 KB** (`CONFIG_ESP32S3_DATA_CACHE_64KB=y`): another +8 / +6 / +7 % on top of the 64 B line, for 32 KB of
  internal RAM (31,996 B less heap, measured).
- **Decoder code in IRAM** (`CONFIG_VJ_IRAM=y`, the default): 50,831 B of internal RAM. It gained +1 / +11 / +21 % over flash
  code with the default 16 KB I-cache, and with it, moving the code changed fps by at most 0.11 % on the bench. The alternative, for
  16 KB of internal RAM instead of 51 KB: `CONFIG_VJ_IRAM=n` with `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB=y`, which stayed within
  1 % of IRAM on the bench's code layout (flash code has no layout pin, so treat it as an estimate) (22.17 / 12.60 / 7.63).
- With 64 B lines, a PSRAM buffer that DMA reads or writes needs 64 B alignment in address and size
  (`CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE`; for example `heap_caps_aligned_alloc(64, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_DMA)`
  with `n` a multiple of 64). The library does no DMA, and its own buffers, `f.y`, `f.u` and `f.v` included, are not
  guaranteed line-aligned: do not DMA from the decoded picture, convert it into your own buffer first.

## 5. Contract
- One AU per `vj_decode_au()` call; `vj_au_length()` splits a byte stream. Pictures come out in decode order.
- One decoder per task: calls on one decoder must not overlap.
- `f.y`, `f.u` and `f.v` are valid until the next `vj_decode_au()` or `vj_decoder_destroy()`.
- The call rewrites the AU in place (it removes emulation-prevention bytes): pass writable RAM, never flash-mapped data, and
  decode each AU once.
- `f.changed_mb` has one byte per 16x16 macroblock of the coded frame, `(width / 16) x (height / 16)`, row by row, valid
  as long as `f.y`. 0 means the macroblock is bit-identical to the same macroblock in the previous `VJ_FRAME` this
  decoder returned, so a consumer that converted that frame can skip it; 1 means it may differ. Frames decoded in between
  but never returned (an error call, a dropped concealed picture) are accounted for. If you skipped a frame yourself,
  treat every macroblock of the next one as changed. NULL means the same as all 1. The first frame, an IDR, and a frame
  with concealed macroblocks have every byte at 1.
- Frames are unreliable until one with `f.idr` arrives: before the first IDR of a stream (a P-frame can decode as a
  clean-looking `VJ_FRAME`) and after any `VJ_ERR_*` (a lost reference can leave `error_mbs` at 0). After an error,
  request a keyframe. `error_mbs` counts concealed macroblocks in a returned frame and is rarely non-zero, but
  `error_mbs` > 0 on a returned frame is also a keyframe cue: the frame is damaged, so request one and show nothing
  until the next IDR.

## 6. Kconfig (menu "VideoJet")
- `CONFIG_VJ_IRAM` (default y): run the decoder from IRAM, 50,831 B of internal RAM (section 4).
- `CONFIG_VJ_ERROR_PRINT` (default n): print the decoder's bitstream errors to stderr (about 60 messages, from the decoding task).
- `CONFIG_VJ_PROFILE` (default n): per-stage cycle counters, read with `vj_profile_take()`. Their cost is not separable from code-layout
  noise (`docs/bench-log.md`) and they move code layout: profiling builds only, never timed scores (the bench enables it
  through `bench/sdkconfig.profile`).

## 7. Licensing
The h264bsd core is Apache-2.0 (`h264bsd/LICENSE.md`). The licence for VideoJet's own code is to be chosen by the owner
before publishing.
